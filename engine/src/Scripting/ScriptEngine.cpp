/*
 * ScriptEngine — Implementation of DLL loading, hot-reload, instance management,
 * script project management, registry generation, and auto-compile.
 */
#include "VibeEngine/Scripting/ScriptEngine.h"
#include "VibeEngine/Scripting/NativeScript.h"
#include "VibeEngine/Scene/Scene.h"
#include "VibeEngine/Scene/SceneManager.h"
#include "VibeEngine/Scene/Entity.h"
#include "VibeEngine/Scene/Components.h"
#include "VibeEngine/Asset/FileWatcher.h"
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
    SceneManager* ActiveSceneManager = nullptr;

    // API table
    ScriptAPI APITable;

    // Reload flag
    bool ReloadedThisFrame = false;

    // Mutex protecting DLL load/unload/reload operations
    std::mutex ReloadMutex;

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

    // File watcher for script sources
    FileWatcher ScriptFileWatcher;
    bool ScriptFileWatcherInitialized = false;

    // Debounce state for auto-build-on-save
    float SourceChangeTimer = -1.0f;
    bool SourceFilesAdded = false;    // file created/deleted → need registry regen + configure
    bool SourceFilesModified = false; // file modified → just rebuild
    static constexpr float DebounceDelay = 0.3f;

    // Auto-reload pipeline
    bool NeedsConfigure = true;
    bool AutoReloadAfterBuild = false;
    bool BuildPendingAfterCurrent = false;
    bool PlayMode = false;
    bool AutoReloadEnabled = true;
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
    if (!s_Data) return;
    s_Data->ReloadedThisFrame = false;

    // Path 1: Auto-reload after successful async build
    if (s_Data->AutoReloadAfterBuild && s_Data->BuildState == BuildStatus::Success) {
        s_Data->AutoReloadAfterBuild = false;
        std::string dllPath;
        {
            std::lock_guard<std::mutex> lock(s_Data->BuildMutex);
            dllPath = s_Data->BuildDLLOutputPath;
        }
        if (!dllPath.empty() && std::filesystem::exists(dllPath)) {
            std::lock_guard<std::mutex> lock(s_Data->ReloadMutex);
            s_Data->DLLPath = dllPath;
            if (s_Data->PlayMode && s_Data->ActiveScene) {
                ReloadScripts(*s_Data->ActiveScene);
            } else if (s_Data->DLLHandle) {
                ReloadDLLOnly();
            } else {
                LoadScriptDLL(dllPath);
            }
            s_Data->ReloadedThisFrame = true;
            VE_ENGINE_INFO("Auto-reload after build complete");
        }
    }

    // Handle pending build (queued while previous build was running)
    if (s_Data->BuildPendingAfterCurrent && s_Data->BuildState != BuildStatus::Building) {
        s_Data->BuildPendingAfterCurrent = false;
        s_Data->AutoReloadAfterBuild = true;
        BuildScriptProject();
    }

    // Clear auto-reload flag on build failure
    if (s_Data->AutoReloadAfterBuild && s_Data->BuildState == BuildStatus::Failed) {
        s_Data->AutoReloadAfterBuild = false;
    }

    // Path 2: DLL file timestamp changed (fallback / external rebuild)
    // Skip if we're waiting for our own build to finish — avoid racing with the build thread
    if (s_Data->AutoReloadAfterBuild || s_Data->BuildState == BuildStatus::Building)
        return;

    if (!s_Data->DLLHandle || s_Data->DLLPath.empty()) return;

    auto srcPath = std::filesystem::path(s_Data->DLLPath);
    if (!std::filesystem::exists(srcPath)) return;

    auto currentTime = std::filesystem::last_write_time(srcPath);
    if (currentTime != s_Data->LastWriteTime) {
        VE_ENGINE_INFO("Script DLL changed (external), hot-reloading...");
        std::lock_guard<std::mutex> lock(s_Data->ReloadMutex);
        if (s_Data->PlayMode && s_Data->ActiveScene)
            ReloadScripts(*s_Data->ActiveScene);
        else
            ReloadDLLOnly();
        s_Data->ReloadedThisFrame = true;
    }
}

void ScriptEngine::ReloadScripts(Scene& scene) {
    // NOTE: Caller must hold s_Data->ReloadMutex.
    // 1. Save properties from all live instances
    struct SavedInstance {
        entt::entity Entity;
        std::string ClassName;
        std::unordered_map<std::string, std::variant<float, int, bool>> Props;
    };
    std::vector<SavedInstance> saved;

    auto view = scene.GetRegistry().view<ScriptComponent>();
    for (auto entity : view) {
        auto& sc = view.get<ScriptComponent>(entity);
        if (sc._Instance) {
            SavedInstance si;
            si.Entity = entity;
            si.ClassName = sc.ClassName;
            ReadPropertiesFromInstance(sc._Instance, sc.ClassName, si.Props);
            saved.push_back(std::move(si));

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

    // 3. Recreate instances and restore properties
    for (auto& si : saved) {
        if (!scene.GetRegistry().valid(si.Entity)) continue;
        auto& sc = scene.GetRegistry().get<ScriptComponent>(si.Entity);
        sc._Instance = CreateInstance(si.ClassName);
        if (sc._Instance) {
            auto& id = scene.GetRegistry().get<IDComponent>(si.Entity);
            sc._Instance->m_EntityID = static_cast<uint64_t>(id.ID);
            ApplyPropertiesToInstance(sc._Instance, si.ClassName, si.Props);
            sc._Instance->OnCreate();
        }
    }

    VE_ENGINE_INFO("Hot-reload complete (properties preserved)");
}

void ScriptEngine::ReloadDLLOnly() {
    // Edit-mode reload: no live instances, just refresh factory map + property cache
    // NOTE: Caller must hold s_Data->ReloadMutex.
    std::string path = s_Data->DLLPath;
    UnloadScriptDLL();
    if (!LoadScriptDLL(path)) {
        VE_ENGINE_ERROR("Edit-mode reload failed!");
        return;
    }
    VE_ENGINE_INFO("Edit-mode hot-reload complete");
}

// ── File watcher + auto-build-on-save ──────────────────────────────

void ScriptEngine::SetupScriptFileWatcher() {
    if (!s_Data) return;

    auto projPath = std::filesystem::absolute(s_Data->ScriptProjectPath).string();
    if (!std::filesystem::exists(projPath)) {
        // Create the directory so the watcher can start
        std::filesystem::create_directories(projPath);
    }

    s_Data->ScriptFileWatcher.Init(projPath, 0.5f);
    s_Data->ScriptFileWatcher.SetCallback([](const std::vector<FileEvent>& events) {
        if (!s_Data || !s_Data->AutoReloadEnabled) return;

        bool relevant = false;
        for (auto& e : events) {
            // Only care about .cpp and .h files
            auto ext = std::filesystem::path(e.FilePath).extension().string();
            if (ext != ".cpp" && ext != ".h") continue;
            // Skip generated files
            auto fname = std::filesystem::path(e.FilePath).filename().string();
            if (fname == "ScriptRegistry.gen.cpp") continue;

            relevant = true;
            if (e.EventType == FileEvent::Type::Created || e.EventType == FileEvent::Type::Deleted)
                s_Data->SourceFilesAdded = true;
            else
                s_Data->SourceFilesModified = true;
        }

        if (relevant) {
            s_Data->SourceChangeTimer = ScriptEngineData::DebounceDelay;
            VE_ENGINE_INFO("Script source change detected, will build in {0}ms...",
                (int)(ScriptEngineData::DebounceDelay * 1000));
        }
    });

    s_Data->ScriptFileWatcherInitialized = true;
    VE_ENGINE_INFO("Script file watcher started on: {0}", projPath);
}

void ScriptEngine::UpdateScriptFileWatcher(float dt) {
    if (!s_Data || !s_Data->ScriptFileWatcherInitialized) return;

    s_Data->ScriptFileWatcher.Update(dt);

    // Debounce timer
    if (s_Data->SourceChangeTimer > 0.0f) {
        s_Data->SourceChangeTimer -= dt;
        if (s_Data->SourceChangeTimer <= 0.0f) {
            s_Data->SourceChangeTimer = -1.0f;

            // Files added/deleted → regenerate registry, need configure
            if (s_Data->SourceFilesAdded) {
                RegenerateScriptRegistry();
                s_Data->NeedsConfigure = true;
            }
            s_Data->SourceFilesAdded = false;
            s_Data->SourceFilesModified = false;

            // Trigger build
            if (IsBuildInProgress()) {
                s_Data->BuildPendingAfterCurrent = true;
                VE_ENGINE_INFO("Build already in progress, queuing next build...");
            } else {
                s_Data->AutoReloadAfterBuild = true;
                BuildScriptProject();
            }
        }
    }
}

void ScriptEngine::RequestBuildAndReload() {
    if (!s_Data || IsBuildInProgress()) return;
    RegenerateScriptRegistry();
    s_Data->AutoReloadAfterBuild = true;
    BuildScriptProject();
    VE_ENGINE_INFO("Manual build+reload triggered (Ctrl+R)");
}

void ScriptEngine::SetPlayMode(bool playing) {
    if (s_Data) s_Data->PlayMode = playing;
}

void ScriptEngine::SetAutoReloadEnabled(bool enabled) {
    if (s_Data) s_Data->AutoReloadEnabled = enabled;
}

bool ScriptEngine::IsAutoReloadEnabled() {
    return s_Data && s_Data->AutoReloadEnabled;
}

void ScriptEngine::SetActiveScene(Scene* scene) {
    if (s_Data) s_Data->ActiveScene = scene;
}

Scene* ScriptEngine::GetActiveScene() {
    return s_Data ? s_Data->ActiveScene : nullptr;
}

void ScriptEngine::SetSceneManager(SceneManager* mgr) {
    if (s_Data) s_Data->ActiveSceneManager = mgr;
}

SceneManager* ScriptEngine::GetSceneManager() {
    return s_Data ? s_Data->ActiveSceneManager : nullptr;
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

    // All generated files go into ScriptBuild/ (next to exe, not in Assets)
    auto buildDir = std::filesystem::absolute("ScriptBuild");
    std::filesystem::create_directories(buildDir);

    // Absolute path to Scripts source dir (for CMakeLists references)
    auto absProjPath = std::filesystem::absolute(projPath);
    std::string srcDir = absProjPath.string();
    std::replace(srcDir.begin(), srcDir.end(), '\\', '/');

    // Generate ScriptRegistry.gen.cpp in ScriptBuild/
    {
        auto registryPath = buildDir / "ScriptRegistry.gen.cpp";
        std::ofstream fout(registryPath);
        fout << "// Auto-generated by VibeEngine — do not edit manually\n";
        fout << "#include <VibeEngine/Scripting/NativeScript.h>\n\n";
        fout << "// Static member definition for DLL (not linked against engine)\n";
        fout << "const VE::ScriptAPI* VE::NativeScript::API = nullptr;\n\n";

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

    // Generate CMakeLists.txt in ScriptBuild/
    {
        auto cmakePath = buildDir / "CMakeLists.txt";
        std::ofstream fout(cmakePath);
        fout << "# Auto-generated by VibeEngine — do not edit manually\n";
        fout << "cmake_minimum_required(VERSION 3.20)\n";
        fout << "project(GameScripts LANGUAGES CXX)\n";
        fout << "set(CMAKE_CXX_STANDARD 17)\n\n";

        // Source files: user scripts with absolute paths + local registry
        fout << "add_library(GameScripts SHARED\n";
        for (auto& f : cppFiles) {
            fout << "    \"" << srcDir << "/" << f << "\"\n";
        }
        fout << "    \"${CMAKE_SOURCE_DIR}/ScriptRegistry.gen.cpp\"\n";
        fout << ")\n\n";

        fout << "target_include_directories(GameScripts PRIVATE\n";
        if (s_Data && !s_Data->EngineIncludePath.empty()) {
            std::string incPath = s_Data->EngineIncludePath;
            std::replace(incPath.begin(), incPath.end(), '\\', '/');
            fout << "    \"" << incPath << "\"\n";
        } else {
            fout << "    ${CMAKE_SOURCE_DIR}/../engine/include\n";
        }
        fout << ")\n\n";

        fout << "target_compile_definitions(GameScripts PRIVATE VE_SCRIPT_DLL)\n";
        fout.close();
        VE_ENGINE_INFO("Generated CMakeLists.txt with {0} files", cppFiles.size() + 1);
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

    s_Data->BuildState = BuildStatus::Building;
    {
        std::lock_guard<std::mutex> lock(s_Data->BuildMutex);
        s_Data->BuildOutput.clear();
    }

    // Detach any previous thread
    if (s_Data->BuildThread.joinable())
        s_Data->BuildThread.join();

    std::string cmakeBin = "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\Common7\\IDE\\CommonExtensions\\Microsoft\\CMake\\CMake\\bin\\cmake.exe";

    // Build directory next to exe, not inside Assets
    std::string buildDir = std::filesystem::absolute("ScriptBuild").string();
    bool needsConfigure = s_Data->NeedsConfigure;

    // Check if out/ directory exists — if not, must configure
    if (!std::filesystem::exists(buildDir + "/out")) {
        needsConfigure = true;
    }

    // If configure needed, ensure registry is up to date
    if (needsConfigure) {
        RegenerateScriptRegistry();
    }

    s_Data->BuildThread = std::thread([cmakeBin, buildDir, needsConfigure]() {
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

        // Configure — only when needed (first build or files added/removed)
        if (needsConfigure) {
            std::string configCmd = "\"" + cmakeBin + "\" -B \"" + buildDir + "/out\" -S \"" + buildDir + "\" -G \"Visual Studio 17 2022\" -A x64";
            output += "=== Configuring ===\n";
            int configResult = runCmd(configCmd);
            if (configResult != 0) {
                output += "\n=== Configure FAILED ===\n";
                {
                    std::lock_guard<std::mutex> lock(s_Data->BuildMutex);
                    s_Data->BuildOutput = output;
                }
                s_Data->BuildState = ScriptEngine::BuildStatus::Failed;
                return;
            }
            s_Data->NeedsConfigure = false;
        } else {
            output += "=== Skipping configure (incremental) ===\n";
        }

        // Build (incremental — only recompiles changed files)
        std::string buildCmd = "\"" + cmakeBin + "\" --build \"" + buildDir + "/out\" --config Debug";
        output += "\n=== Building ===\n";
        int buildResult = runCmd(buildCmd);
        if (buildResult != 0) {
            output += "\n=== Build FAILED ===\n";
            {
                std::lock_guard<std::mutex> lock(s_Data->BuildMutex);
                s_Data->BuildOutput = output;
            }
            s_Data->BuildState = ScriptEngine::BuildStatus::Failed;
            return;
        }

        output += "\n=== Build SUCCESS ===\n";

        std::string dllPath = buildDir + "/out/Debug/GameScripts.dll";
        {
            std::lock_guard<std::mutex> lock(s_Data->BuildMutex);
            s_Data->BuildOutput = output;
            s_Data->BuildDLLOutputPath = dllPath;
        }
        s_Data->BuildState = ScriptEngine::BuildStatus::Success;
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
    std::string buildDir = std::filesystem::absolute("ScriptBuild").string();
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

    // Configure — only when needed
    bool needsConfigure = s_Data->NeedsConfigure || !std::filesystem::exists(buildDir + "/out");
    if (needsConfigure) {
        std::string configCmd = "\"" + cmakeBin + "\" -B \"" + buildDir + "/out\" -S \"" + buildDir + "\" -G \"Visual Studio 17 2022\" -A x64";
        VE_ENGINE_INFO("CMake cmd: {0}", configCmd);
        int configResult = runCmd(configCmd);
        if (configResult != 0) {
            VE_ENGINE_ERROR("Script project configure failed (exit {0})", configResult);
            VE_ENGINE_ERROR("Output:\n{0}", output);
            s_Data->BuildOutput = output;
            s_Data->BuildState = BuildStatus::Failed;
            return false;
        }
        s_Data->NeedsConfigure = false;
    }

    // Build (incremental)
    std::string buildCmd = "\"" + cmakeBin + "\" --build \"" + buildDir + "/out\" --config Debug";
    if (runCmd(buildCmd) != 0) {
        VE_ENGINE_ERROR("Script project build failed");
        s_Data->BuildOutput = output;
        s_Data->BuildState = BuildStatus::Failed;
        return false;
    }

    s_Data->BuildOutput = output;
    s_Data->BuildState = BuildStatus::Success;

    // Auto-load
    std::string dllPath = buildDir + "/out/Debug/GameScripts.dll";
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
