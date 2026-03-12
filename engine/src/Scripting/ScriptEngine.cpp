/*
 * ScriptEngine — Implementation of DLL loading, hot-reload, instance management,
 * script project management, registry generation, and auto-compile.
 */
#include "VibeEngine/Scripting/ScriptEngine.h"
#include "VibeEngine/Scripting/NativeScript.h"
#include "VibeEngine/Scene/Scene.h"
#include "VibeEngine/Scene/Entity.h"
#include "VibeEngine/Scene/Components.h"
#include "VibeEngine/Core/Log.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

#include <filesystem>
#include <unordered_map>
#include <functional>
#include <fstream>
#include <sstream>
#include <regex>
#include <thread>
#include <mutex>
#include <atomic>

namespace VE {

// Static API table instance — set once at Init
const ScriptAPI* NativeScript::API = nullptr;

// ── Internal state ──────────────────────────────────────────────────

struct ScriptEngineData {
    // DLL handle
#ifdef _WIN32
    HMODULE DLLHandle = nullptr;
#endif
    std::string DLLPath;
    std::string TempDLLPath;
    std::filesystem::file_time_type LastWriteTime;

    // Factory table from DLL
    using GetEntriesFn = ScriptEntry* (*)(int*);
    using SetAPIFn     = void (*)(const ScriptAPI*);

    GetEntriesFn GetEntries = nullptr;
    std::unordered_map<std::string, std::function<NativeScript*()>> Factories;

    // Scene context
    Scene* ActiveScene = nullptr;

    // API table
    ScriptAPI APITable;

    // Reload flag
    bool ReloadedThisFrame = false;

    // Property metadata cache (populated at DLL load)
    std::unordered_map<std::string, std::vector<ScriptEngine::CachedProperty>> PropertyCache;

    // Script project
    std::string ScriptProjectPath = "Assets/Scripts";
    std::string EngineIncludePath; // absolute path to engine/include (set at init)

    // Build system
    std::atomic<ScriptEngine::BuildStatus> BuildState{ScriptEngine::BuildStatus::Idle};
    std::string BuildOutput;
    std::string BuildDLLOutputPath;
    std::mutex BuildMutex;
    std::thread BuildThread;
};

static ScriptEngineData* s_Data = nullptr;

// ── ScriptGlue — API function implementations (defined in ScriptGlue.cpp) ──
extern void InitScriptGlue(ScriptAPI& api);

// ── Public API ──────────────────────────────────────────────────────

void ScriptEngine::Init() {
    s_Data = new ScriptEngineData();
    InitScriptGlue(s_Data->APITable);
    NativeScript::API = &s_Data->APITable;
    VE_ENGINE_INFO("ScriptEngine initialized");
}

void ScriptEngine::Shutdown() {
    // Wait for any in-progress build
    if (s_Data && s_Data->BuildThread.joinable())
        s_Data->BuildThread.join();

    UnloadScriptDLL();
    NativeScript::API = nullptr;
    delete s_Data;
    s_Data = nullptr;
    VE_ENGINE_INFO("ScriptEngine shutdown");
}

bool ScriptEngine::LoadScriptDLL(const std::string& path) {
#ifdef _WIN32
    if (s_Data->DLLHandle)
        UnloadScriptDLL();

    s_Data->DLLPath = path;

    // Copy DLL to temp so user can keep rebuilding
    auto srcPath = std::filesystem::path(path);
    if (!std::filesystem::exists(srcPath)) {
        VE_ENGINE_ERROR("Script DLL not found: {0}", path);
        return false;
    }

    s_Data->LastWriteTime = std::filesystem::last_write_time(srcPath);

    // Temp copy next to original with __loaded suffix
    auto tempPath = srcPath.parent_path() / (srcPath.stem().string() + "__loaded.dll");
    std::error_code ec;
    std::filesystem::copy_file(srcPath, tempPath,
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        VE_ENGINE_ERROR("Failed to copy script DLL: {0}", ec.message());
        return false;
    }
    s_Data->TempDLLPath = tempPath.string();

    // Load the temp copy
    s_Data->DLLHandle = LoadLibraryA(s_Data->TempDLLPath.c_str());
    if (!s_Data->DLLHandle) {
        VE_ENGINE_ERROR("Failed to load script DLL: {0} (error {1})",
            s_Data->TempDLLPath, GetLastError());
        return false;
    }

    // Get exported functions
    auto getEntries = (ScriptEngineData::GetEntriesFn)GetProcAddress(
        s_Data->DLLHandle, "GetScriptEntries");
    auto setAPI = (ScriptEngineData::SetAPIFn)GetProcAddress(
        s_Data->DLLHandle, "SetScriptAPI");

    if (!getEntries || !setAPI) {
        VE_ENGINE_ERROR("Script DLL missing required exports (GetScriptEntries/SetScriptAPI)");
        FreeLibrary(s_Data->DLLHandle);
        s_Data->DLLHandle = nullptr;
        return false;
    }

    // Pass API table to DLL
    setAPI(&s_Data->APITable);

    // Build factory map
    s_Data->Factories.clear();
    int count = 0;
    ScriptEntry* entries = getEntries(&count);
    for (int i = 0; i < count; i++) {
        s_Data->Factories[entries[i].Name] = entries[i].CreateFunc;
        VE_ENGINE_INFO("  Registered script: {0}", entries[i].Name);
    }

    // Cache property metadata by creating temp instances
    s_Data->PropertyCache.clear();
    for (auto& [name, factory] : s_Data->Factories) {
        NativeScript* temp = factory();
        if (temp) {
            int propCount = 0;
            auto* props = temp->GetProperties(propCount);
            std::vector<CachedProperty> cached;
            for (int i = 0; i < propCount; i++) {
                CachedProperty cp;
                cp.Name = props[i].Name;
                cp.Type = static_cast<int>(props[i].Type);
                cp.Offset = props[i].Offset;
                // Read default values from temp instance
                char* base = reinterpret_cast<char*>(temp);
                switch (props[i].Type) {
                    case ScriptPropertyType::Float:
                        cp.DefaultFloat = *reinterpret_cast<float*>(base + props[i].Offset);
                        break;
                    case ScriptPropertyType::Int:
                        cp.DefaultInt = *reinterpret_cast<int*>(base + props[i].Offset);
                        break;
                    case ScriptPropertyType::Bool:
                        cp.DefaultBool = *reinterpret_cast<bool*>(base + props[i].Offset);
                        break;
                    default: break;
                }
                cached.push_back(cp);
            }
            s_Data->PropertyCache[name] = std::move(cached);
            delete temp;
        }
    }

    VE_ENGINE_INFO("Script DLL loaded: {0} ({1} scripts)", path, count);
    return true;
#else
    VE_ENGINE_ERROR("Script DLL loading not supported on this platform");
    return false;
#endif
}

void ScriptEngine::UnloadScriptDLL() {
#ifdef _WIN32
    if (!s_Data || !s_Data->DLLHandle) return;

    s_Data->Factories.clear();

    FreeLibrary(s_Data->DLLHandle);
    s_Data->DLLHandle = nullptr;

    // Clean up temp DLL
    if (!s_Data->TempDLLPath.empty()) {
        std::error_code ec;
        std::filesystem::remove(s_Data->TempDLLPath, ec);
        s_Data->TempDLLPath.clear();
    }

    VE_ENGINE_INFO("Script DLL unloaded");
#endif
}

bool ScriptEngine::IsLoaded() {
#ifdef _WIN32
    return s_Data && s_Data->DLLHandle != nullptr;
#else
    return false;
#endif
}

NativeScript* ScriptEngine::CreateInstance(const std::string& className) {
    if (!s_Data) return nullptr;

    auto it = s_Data->Factories.find(className);
    if (it == s_Data->Factories.end()) {
        VE_ENGINE_WARN("Script class not found: {0}", className);
        return nullptr;
    }

    return it->second();
}

void ScriptEngine::DestroyInstance(NativeScript* instance) {
    delete instance;
}

void ScriptEngine::CheckForReload() {
    s_Data->ReloadedThisFrame = false;

    if (!s_Data || !s_Data->DLLHandle || s_Data->DLLPath.empty())
        return;

    auto srcPath = std::filesystem::path(s_Data->DLLPath);
    if (!std::filesystem::exists(srcPath))
        return;

    auto currentTime = std::filesystem::last_write_time(srcPath);
    if (currentTime != s_Data->LastWriteTime) {
        VE_ENGINE_INFO("Script DLL changed, hot-reloading...");
        if (s_Data->ActiveScene)
            ReloadScripts(*s_Data->ActiveScene);
        s_Data->ReloadedThisFrame = true;
    }
}

void ScriptEngine::ReloadScripts(Scene& scene) {
    // 1. Destroy all existing script instances
    auto view = scene.GetRegistry().view<ScriptComponent>();
    for (auto entity : view) {
        auto& sc = view.get<ScriptComponent>(entity);
        if (sc._Instance) {
            sc._Instance->OnDestroy();
            DestroyInstance(sc._Instance);
            sc._Instance = nullptr;
        }
    }

    // 2. Reload DLL
    std::string path = s_Data->DLLPath;
    UnloadScriptDLL();
    if (!LoadScriptDLL(path)) {
        VE_ENGINE_ERROR("Hot-reload failed!");
        return;
    }

    // 3. Recreate instances
    for (auto entity : view) {
        auto& sc = view.get<ScriptComponent>(entity);
        if (sc.ClassName.empty()) continue;

        sc._Instance = CreateInstance(sc.ClassName);
        if (sc._Instance) {
            auto& id = scene.GetRegistry().get<IDComponent>(entity);
            sc._Instance->m_EntityID = static_cast<uint64_t>(id.ID);
            sc._Instance->OnCreate();
        }
    }

    VE_ENGINE_INFO("Hot-reload complete");
}

void ScriptEngine::SetActiveScene(Scene* scene) {
    if (s_Data) s_Data->ActiveScene = scene;
}

Scene* ScriptEngine::GetActiveScene() {
    return s_Data ? s_Data->ActiveScene : nullptr;
}

std::string ScriptEngine::GetDLLPath() {
    return s_Data ? s_Data->DLLPath : "";
}

std::vector<std::string> ScriptEngine::GetRegisteredClassNames() {
    std::vector<std::string> names;
    if (s_Data) {
        for (auto& [name, _] : s_Data->Factories)
            names.push_back(name);
    }
    return names;
}

bool ScriptEngine::WasReloadedThisFrame() {
    return s_Data && s_Data->ReloadedThisFrame;
}

std::vector<ScriptEngine::CachedProperty> ScriptEngine::GetScriptProperties(const std::string& className) {
    if (!s_Data) return {};
    auto it = s_Data->PropertyCache.find(className);
    if (it != s_Data->PropertyCache.end())
        return it->second;
    return {};
}

void ScriptEngine::ApplyPropertiesToInstance(NativeScript* instance, const std::string& className,
    const std::unordered_map<std::string, std::variant<float, int, bool>>& values)
{
    if (!instance || !s_Data) return;
    auto propsIt = s_Data->PropertyCache.find(className);
    if (propsIt == s_Data->PropertyCache.end()) return;

    char* base = reinterpret_cast<char*>(instance);
    for (auto& cp : propsIt->second) {
        auto valIt = values.find(cp.Name);
        if (valIt == values.end()) continue;
        switch (static_cast<ScriptPropertyType>(cp.Type)) {
            case ScriptPropertyType::Float:
                if (auto* v = std::get_if<float>(&valIt->second))
                    *reinterpret_cast<float*>(base + cp.Offset) = *v;
                break;
            case ScriptPropertyType::Int:
                if (auto* v = std::get_if<int>(&valIt->second))
                    *reinterpret_cast<int*>(base + cp.Offset) = *v;
                break;
            case ScriptPropertyType::Bool:
                if (auto* v = std::get_if<bool>(&valIt->second))
                    *reinterpret_cast<bool*>(base + cp.Offset) = *v;
                break;
            default: break;
        }
    }
}

void ScriptEngine::ReadPropertiesFromInstance(NativeScript* instance, const std::string& className,
    std::unordered_map<std::string, std::variant<float, int, bool>>& values)
{
    if (!instance || !s_Data) return;
    auto propsIt = s_Data->PropertyCache.find(className);
    if (propsIt == s_Data->PropertyCache.end()) return;

    char* base = reinterpret_cast<char*>(instance);
    for (auto& cp : propsIt->second) {
        switch (static_cast<ScriptPropertyType>(cp.Type)) {
            case ScriptPropertyType::Float:
                values[cp.Name] = *reinterpret_cast<float*>(base + cp.Offset);
                break;
            case ScriptPropertyType::Int:
                values[cp.Name] = *reinterpret_cast<int*>(base + cp.Offset);
                break;
            case ScriptPropertyType::Bool:
                values[cp.Name] = *reinterpret_cast<bool*>(base + cp.Offset);
                break;
            default: break;
        }
    }
}

// ── Script project management ───────────────────────────────────────

void ScriptEngine::SetScriptProjectPath(const std::string& path) {
    if (s_Data) s_Data->ScriptProjectPath = path;
}

std::string ScriptEngine::GetScriptProjectPath() {
    return s_Data ? s_Data->ScriptProjectPath : "Assets/Scripts";
}

void ScriptEngine::SetEngineIncludePath(const std::string& path) {
    if (s_Data) s_Data->EngineIncludePath = path;
}

std::vector<std::string> ScriptEngine::ScanScriptClassNames() {
    std::vector<std::string> scriptNames;
    if (!s_Data) return scriptNames;

    auto projPath = std::filesystem::path(s_Data->ScriptProjectPath);
    if (!std::filesystem::exists(projPath)) return scriptNames;

    std::regex registerRegex(R"(REGISTER_SCRIPT\(\s*(\w+)\s*\))");

    for (auto& entry : std::filesystem::directory_iterator(projPath)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension().string() != ".cpp") continue;
        if (entry.path().filename().string() == "ScriptRegistry.gen.cpp") continue;

        std::ifstream fin(entry.path());
        std::string content((std::istreambuf_iterator<char>(fin)),
                             std::istreambuf_iterator<char>());
        fin.close();

        std::sregex_iterator it(content.begin(), content.end(), registerRegex);
        std::sregex_iterator end;
        for (; it != end; ++it)
            scriptNames.push_back((*it)[1].str());
    }
    return scriptNames;
}

bool ScriptEngine::HasScriptFiles() {
    if (!s_Data) return false;
    auto projPath = std::filesystem::path(s_Data->ScriptProjectPath);
    if (!std::filesystem::exists(projPath)) return false;
    for (auto& entry : std::filesystem::directory_iterator(projPath)) {
        if (entry.is_regular_file() && entry.path().extension() == ".cpp"
            && entry.path().filename().string() != "ScriptRegistry.gen.cpp")
            return true;
    }
    return false;
}

bool ScriptEngine::CreateNewScript(const std::string& className) {
    if (!s_Data) return false;

    auto projPath = std::filesystem::path(s_Data->ScriptProjectPath);
    if (!std::filesystem::exists(projPath)) {
        std::filesystem::create_directories(projPath);
    }

    auto filePath = projPath / (className + ".cpp");
    if (std::filesystem::exists(filePath)) {
        VE_ENGINE_WARN("Script file already exists: {0}", filePath.string());
        return false;
    }

    // Write clean script template
    std::ofstream fout(filePath);
    if (!fout.is_open()) {
        VE_ENGINE_ERROR("Failed to create script file: {0}", filePath.string());
        return false;
    }

    fout << "#include <VibeEngine/Scripting/NativeScript.h>\n"
         << "using namespace VE;\n\n"
         << "class " << className << " : public NativeScript {\n"
         << "public:\n"
         << "    void OnCreate() override {\n"
         << "    }\n\n"
         << "    void OnUpdate(float dt) override {\n"
         << "    }\n\n"
         << "    void OnDestroy() override {\n"
         << "    }\n"
         << "};\n\n"
         << "REGISTER_SCRIPT(" << className << ")\n";
    fout.close();

    VE_ENGINE_INFO("Created new script: {0}", filePath.string());

    // Regenerate registry and CMakeLists
    RegenerateScriptRegistry();
    return true;
}

void ScriptEngine::RegenerateScriptRegistry() {
    if (!s_Data) return;

    auto projPath = std::filesystem::path(s_Data->ScriptProjectPath);
    if (!std::filesystem::exists(projPath)) return;

    // Scan all .cpp files (except ScriptRegistry.gen.cpp) for REGISTER_SCRIPT(XXX)
    std::vector<std::string> scriptNames;
    std::vector<std::string> cppFiles;
    std::regex registerRegex(R"(REGISTER_SCRIPT\(\s*(\w+)\s*\))");

    for (auto& entry : std::filesystem::directory_iterator(projPath)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        if (ext != ".cpp") continue;
        auto filename = entry.path().filename().string();
        if (filename == "ScriptRegistry.gen.cpp") continue;

        cppFiles.push_back(filename);

        // Read file and find REGISTER_SCRIPT
        std::ifstream fin(entry.path());
        std::string content((std::istreambuf_iterator<char>(fin)),
                             std::istreambuf_iterator<char>());
        fin.close();

        std::sregex_iterator it(content.begin(), content.end(), registerRegex);
        std::sregex_iterator end;
        for (; it != end; ++it) {
            scriptNames.push_back((*it)[1].str());
        }
    }

    // Generate ScriptRegistry.gen.cpp
    {
        auto registryPath = projPath / "ScriptRegistry.gen.cpp";
        std::ofstream fout(registryPath);
        fout << "// Auto-generated by VibeEngine — do not edit manually\n";
        fout << "#include <VibeEngine/Scripting/NativeScript.h>\n\n";
        fout << "// Static member definition for DLL (not linked against engine)\n";
        fout << "const VE::ScriptAPI* VE::NativeScript::API = nullptr;\n\n";

        // Forward-declare factory functions
        for (auto& name : scriptNames) {
            fout << "extern VE::NativeScript* Create_" << name << "();\n";
        }

        fout << "\nstatic ScriptEntry s_Scripts[] = {\n";
        for (size_t i = 0; i < scriptNames.size(); i++) {
            fout << "    { \"" << scriptNames[i] << "\", Create_" << scriptNames[i] << " }";
            if (i + 1 < scriptNames.size()) fout << ",";
            fout << "\n";
        }
        if (scriptNames.empty()) {
            fout << "    { nullptr, nullptr } // placeholder\n";
        }
        fout << "};\n\n";

        fout << "extern \"C\" VE_SCRIPT_API ScriptEntry* GetScriptEntries(int* count) {\n";
        fout << "    *count = " << scriptNames.size() << ";\n";
        fout << "    return s_Scripts;\n";
        fout << "}\n\n";

        fout << "extern \"C\" VE_SCRIPT_API void SetScriptAPI(const VE::ScriptAPI* api) {\n";
        fout << "    VE::NativeScript::SetAPI(api);\n";
        fout << "}\n";
        fout.close();
        VE_ENGINE_INFO("Generated ScriptRegistry.gen.cpp with {0} scripts", scriptNames.size());
    }

    // Add ScriptRegistry.gen.cpp to file list
    cppFiles.push_back("ScriptRegistry.gen.cpp");

    // Generate CMakeLists.txt
    {
        auto cmakePath = projPath / "CMakeLists.txt";
        std::ofstream fout(cmakePath);
        fout << "# Auto-generated by VibeEngine — do not edit manually\n";
        fout << "cmake_minimum_required(VERSION 3.20)\n";
        fout << "project(GameScripts LANGUAGES CXX)\n";
        fout << "set(CMAKE_CXX_STANDARD 17)\n\n";
        fout << "add_library(GameScripts SHARED\n";
        for (auto& f : cppFiles) {
            fout << "    " << f << "\n";
        }
        fout << ")\n\n";
        fout << "target_include_directories(GameScripts PRIVATE\n";
        // Use absolute engine include path if set, otherwise fallback to relative
        if (s_Data && !s_Data->EngineIncludePath.empty()) {
            // Convert backslashes to forward slashes for CMake
            std::string incPath = s_Data->EngineIncludePath;
            std::replace(incPath.begin(), incPath.end(), '\\', '/');
            fout << "    \"" << incPath << "\"\n";
        } else {
            fout << "    ${CMAKE_SOURCE_DIR}/../engine/include\n";
        }
        fout << ")\n\n";
        fout << "target_compile_definitions(GameScripts PRIVATE VE_SCRIPT_DLL)\n";
        fout.close();
        VE_ENGINE_INFO("Generated CMakeLists.txt with {0} files", cppFiles.size());
    }
}

// ── Auto-compile ────────────────────────────────────────────────────

bool ScriptEngine::BuildScriptProject() {
    if (!s_Data) return false;
    if (s_Data->BuildState == BuildStatus::Building) return false;

    auto projPath = std::filesystem::absolute(s_Data->ScriptProjectPath).string();
    if (!std::filesystem::exists(projPath)) {
        VE_ENGINE_ERROR("Script project not found: {0}", projPath);
        return false;
    }

    // Ensure registry is up to date
    RegenerateScriptRegistry();

    s_Data->BuildState = BuildStatus::Building;
    {
        std::lock_guard<std::mutex> lock(s_Data->BuildMutex);
        s_Data->BuildOutput.clear();
    }

    // Detach any previous thread
    if (s_Data->BuildThread.joinable())
        s_Data->BuildThread.join();

    std::string cmakeBin = "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\Common7\\IDE\\CommonExtensions\\Microsoft\\CMake\\CMake\\bin\\cmake.exe";

    s_Data->BuildThread = std::thread([projPath, cmakeBin]() {
        std::string output;

        // Helper to run command and capture output
        auto runCmd = [&](const std::string& cmd) -> int {
            std::string fullCmd = "\"" + cmd + "\" 2>&1";
            FILE* pipe = _popen(fullCmd.c_str(), "r");
            if (!pipe) {
                output += "ERROR: Failed to run: " + cmd + "\n";
                return -1;
            }
            char buf[256];
            while (fgets(buf, sizeof(buf), pipe))
                output += buf;
            return _pclose(pipe);
        };

        // Configure
        std::string configCmd = "\"" + cmakeBin + "\" -B \"" + projPath + "/build\" -S \"" + projPath + "\" -G \"Visual Studio 17 2022\" -A x64";
        output += "=== Configuring ===\n";
        int configResult = runCmd(configCmd);
        if (configResult != 0) {
            output += "\n=== Configure FAILED ===\n";
            {
                std::lock_guard<std::mutex> lock(s_Data->BuildMutex);
                s_Data->BuildOutput = output;
            }
            s_Data->BuildState = BuildStatus::Failed;
            return;
        }

        // Build
        std::string buildCmd = "\"" + cmakeBin + "\" --build \"" + projPath + "/build\" --config Debug";
        output += "\n=== Building ===\n";
        int buildResult = runCmd(buildCmd);
        if (buildResult != 0) {
            output += "\n=== Build FAILED ===\n";
            {
                std::lock_guard<std::mutex> lock(s_Data->BuildMutex);
                s_Data->BuildOutput = output;
            }
            s_Data->BuildState = BuildStatus::Failed;
            return;
        }

        output += "\n=== Build SUCCESS ===\n";

        // Find built DLL
        std::string dllPath = projPath + "/build/Debug/GameScripts.dll";
        {
            std::lock_guard<std::mutex> lock(s_Data->BuildMutex);
            s_Data->BuildOutput = output;
            s_Data->BuildDLLOutputPath = dllPath;
        }
        s_Data->BuildState = BuildStatus::Success;
    });

    return true;
}

bool ScriptEngine::BuildScriptProjectSync() {
    if (!s_Data) return false;

    auto projPath = std::filesystem::absolute(s_Data->ScriptProjectPath).string();
    if (!std::filesystem::exists(projPath)) {
        VE_ENGINE_ERROR("Script project not found: {0}", projPath);
        return false;
    }

    // Check if any .cpp script files exist
    bool hasScripts = false;
    for (auto& entry : std::filesystem::directory_iterator(projPath)) {
        if (entry.is_regular_file() && entry.path().extension() == ".cpp"
            && entry.path().filename().string() != "ScriptRegistry.gen.cpp") {
            hasScripts = true;
            break;
        }
    }
    if (!hasScripts) return false;

    RegenerateScriptRegistry();

    std::string cmakeBin = "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\Common7\\IDE\\CommonExtensions\\Microsoft\\CMake\\CMake\\bin\\cmake.exe";
    std::string output;

    auto runCmd = [&](const std::string& cmd) -> int {
        std::string fullCmd = "\"" + cmd + "\" 2>&1";
        FILE* pipe = _popen(fullCmd.c_str(), "r");
        if (!pipe) {
            output += "ERROR: Failed to run: " + cmd + "\n";
            return -1;
        }
        char buf[256];
        while (fgets(buf, sizeof(buf), pipe))
            output += buf;
        return _pclose(pipe);
    };

    VE_ENGINE_INFO("Building script project (sync)...");

    // Configure
    std::string configCmd = "\"" + cmakeBin + "\" -B \"" + projPath + "/build\" -S \"" + projPath + "\" -G \"Visual Studio 17 2022\" -A x64";
    VE_ENGINE_INFO("CMake cmd: {0}", configCmd);
    int configResult = runCmd(configCmd);
    if (configResult != 0) {
        VE_ENGINE_ERROR("Script project configure failed (exit {0})", configResult);
        VE_ENGINE_ERROR("Output:\n{0}", output);
        s_Data->BuildOutput = output;
        s_Data->BuildState = BuildStatus::Failed;
        return false;
    }

    // Build
    std::string buildCmd = "\"" + cmakeBin + "\" --build \"" + projPath + "/build\" --config Debug";
    if (runCmd(buildCmd) != 0) {
        VE_ENGINE_ERROR("Script project build failed");
        s_Data->BuildOutput = output;
        s_Data->BuildState = BuildStatus::Failed;
        return false;
    }

    s_Data->BuildOutput = output;
    s_Data->BuildState = BuildStatus::Success;

    // Auto-load
    std::string dllPath = projPath + "/build/Debug/GameScripts.dll";
    s_Data->BuildDLLOutputPath = dllPath;
    if (std::filesystem::exists(dllPath)) {
        LoadScriptDLL(dllPath);
        VE_ENGINE_INFO("Script DLL auto-loaded: {0}", dllPath);
        return true;
    }
    return false;
}

bool ScriptEngine::IsBuildInProgress() {
    return s_Data && s_Data->BuildState == BuildStatus::Building;
}

const std::string& ScriptEngine::GetBuildOutput() {
    static std::string empty;
    if (!s_Data) return empty;
    std::lock_guard<std::mutex> lock(s_Data->BuildMutex);
    return s_Data->BuildOutput;
}

const std::string& ScriptEngine::GetBuildDLLOutputPath() {
    static std::string empty;
    if (!s_Data) return empty;
    std::lock_guard<std::mutex> lock(s_Data->BuildMutex);
    return s_Data->BuildDLLOutputPath;
}

ScriptEngine::BuildStatus ScriptEngine::GetBuildStatus() {
    return s_Data ? s_Data->BuildState.load() : BuildStatus::Idle;
}

} // namespace VE
