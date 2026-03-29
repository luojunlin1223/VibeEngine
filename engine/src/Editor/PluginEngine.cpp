/*
 * PluginEngine — Implementation of editor plugin DLL hot-reload system.
 *
 * Mirrors ScriptEngine for DLL management, build system, and file watching,
 * but tailored for editor UI plugins with ImGui context sharing.
 */
#include "VibeEngine/Editor/PluginEngine.h"
#include "VibeEngine/Editor/EditorPlugin.h"
#include "VibeEngine/Asset/FileWatcher.h"
#include "VibeEngine/Core/Log.h"

#include <imgui.h>

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

// ── Internal state ──────────────────────────────────────────────────

struct PluginEngineData {
#ifdef _WIN32
    HMODULE DLLHandle = nullptr;
#endif
    std::string DLLPath;
    std::string TempDLLPath;
    std::filesystem::file_time_type LastWriteTime;

    // Factory table from DLL
    using GetEntriesFn  = PluginEntry* (*)(int*);
    using SetContextFn  = void (*)(void*);  // void* to avoid imgui.h in header
    using SetViewportFn = void (*)(float, float, float, float);

    SetViewportFn SetViewport = nullptr;  // cached function pointer

    // Live plugin instances
    std::vector<EditorPlugin*> ActivePlugins;
    std::vector<std::string> PluginNames;

    // Reload
    std::mutex ReloadMutex;
    bool ReloadedThisFrame = false;

    // Paths
    std::string PluginSourcePath = "Assets/Editor";  // relative to CWD, like ScriptEngine
    std::string EngineIncludePath;
    std::string ImGuiSourcePath;

    // Build system
    std::atomic<PluginEngine::BuildStatus> BuildState{PluginEngine::BuildStatus::Idle};
    std::string BuildOutput;
    std::string BuildDLLOutputPath;
    std::mutex BuildMutex;
    std::thread BuildThread;

    // File watcher
    FileWatcher PluginFileWatcher;
    bool FileWatcherInitialized = false;

    // Debounce
    float SourceChangeTimer = -1.0f;
    bool SourceFilesAdded = false;
    static constexpr float DebounceDelay = 0.3f;

    // Auto-reload pipeline
    bool NeedsConfigure = true;
    bool AutoReloadAfterBuild = false;
    bool BuildPendingAfterCurrent = false;
};

// Static member definition (engine side — DLL has its own copy via generated registry)
PluginViewport EditorPlugin::Viewport = {};

static PluginEngineData* s_Data = nullptr;

// ── Public API ──────────────────────────────────────────────────────

void PluginEngine::Init() {
    s_Data = new PluginEngineData();
    VE_ENGINE_INFO("PluginEngine initialized");
}

void PluginEngine::Shutdown() {
    if (s_Data && s_Data->BuildThread.joinable())
        s_Data->BuildThread.join();

    UnloadPluginDLL();
    delete s_Data;
    s_Data = nullptr;
    VE_ENGINE_INFO("PluginEngine shutdown");
}

// ── DLL Loading ─────────────────────────────────────────────────────

bool PluginEngine::LoadPluginDLL(const std::string& path) {
#ifdef _WIN32
    if (!s_Data) return false;
    if (s_Data->DLLHandle)
        UnloadPluginDLL();

    s_Data->DLLPath = path;

    auto srcPath = std::filesystem::path(path);
    if (!std::filesystem::exists(srcPath)) {
        VE_ENGINE_ERROR("Plugin DLL not found: {0}", path);
        return false;
    }

    s_Data->LastWriteTime = std::filesystem::last_write_time(srcPath);

    // Copy to temp so user can keep rebuilding
    auto tempPath = srcPath.parent_path() / (srcPath.stem().string() + "__loaded.dll");
    std::error_code ec;
    std::filesystem::copy_file(srcPath, tempPath,
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        VE_ENGINE_ERROR("Failed to copy plugin DLL: {0}", ec.message());
        return false;
    }
    s_Data->TempDLLPath = tempPath.string();

    s_Data->DLLHandle = LoadLibraryA(s_Data->TempDLLPath.c_str());
    if (!s_Data->DLLHandle) {
        VE_ENGINE_ERROR("Failed to load plugin DLL: {0} (error {1})",
            s_Data->TempDLLPath, GetLastError());
        return false;
    }

    // Get exported functions
    auto getEntries = (PluginEngineData::GetEntriesFn)GetProcAddress(
        s_Data->DLLHandle, "GetPluginEntries");
    auto setContext = (PluginEngineData::SetContextFn)GetProcAddress(
        s_Data->DLLHandle, "SetPluginContext");

    if (!getEntries || !setContext) {
        VE_ENGINE_ERROR("Plugin DLL missing required exports (GetPluginEntries/SetPluginContext)");
        FreeLibrary(s_Data->DLLHandle);
        s_Data->DLLHandle = nullptr;
        return false;
    }

    // Pass ImGui context to DLL
    setContext(ImGui::GetCurrentContext());

    // Get optional viewport setter
    s_Data->SetViewport = (PluginEngineData::SetViewportFn)GetProcAddress(
        s_Data->DLLHandle, "SetPluginViewport");

    // Create plugin instances
    s_Data->ActivePlugins.clear();
    s_Data->PluginNames.clear();
    int count = 0;
    PluginEntry* entries = getEntries(&count);
    for (int i = 0; i < count; i++) {
        EditorPlugin* plugin = entries[i].CreateFunc();
        if (plugin) {
            plugin->OnInit();
            s_Data->ActivePlugins.push_back(plugin);
            s_Data->PluginNames.push_back(entries[i].Name);
            VE_ENGINE_INFO("  Loaded plugin: {0}", entries[i].Name);
        }
    }

    VE_ENGINE_INFO("Plugin DLL loaded: {0} ({1} plugins)", path, count);
    return true;
#else
    VE_ENGINE_ERROR("Plugin DLL loading not supported on this platform");
    return false;
#endif
}

void PluginEngine::UnloadPluginDLL() {
#ifdef _WIN32
    if (!s_Data || !s_Data->DLLHandle) return;

    // Shutdown and destroy all plugin instances
    for (auto* plugin : s_Data->ActivePlugins) {
        plugin->OnShutdown();
        delete plugin;
    }
    s_Data->ActivePlugins.clear();
    s_Data->PluginNames.clear();

    FreeLibrary(s_Data->DLLHandle);
    s_Data->DLLHandle = nullptr;

    if (!s_Data->TempDLLPath.empty()) {
        std::error_code ec;
        std::filesystem::remove(s_Data->TempDLLPath, ec);
        s_Data->TempDLLPath.clear();
    }

    VE_ENGINE_INFO("Plugin DLL unloaded");
#endif
}

bool PluginEngine::IsLoaded() {
#ifdef _WIN32
    return s_Data && s_Data->DLLHandle != nullptr;
#else
    return false;
#endif
}

void PluginEngine::SetViewportBounds(float x, float y, float w, float h) {
    if (!s_Data) return;
    if (s_Data->SetViewport)
        s_Data->SetViewport(x, y, w, h);
}

void PluginEngine::RenderAllPluginUI() {
    if (!s_Data) return;
    for (auto* plugin : s_Data->ActivePlugins) {
        plugin->OnEditorUI();
    }
}

// ── Hot-Reload ──────────────────────────────────────────────────────

void PluginEngine::CheckForReload() {
    if (!s_Data) return;
    s_Data->ReloadedThisFrame = false;

    // Path 1: Auto-reload after successful build
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
            UnloadPluginDLL();
            LoadPluginDLL(dllPath);
            s_Data->ReloadedThisFrame = true;
            VE_ENGINE_INFO("Plugin auto-reload after build complete");
        }
    }

    // Handle pending build
    if (s_Data->BuildPendingAfterCurrent && s_Data->BuildState != BuildStatus::Building) {
        s_Data->BuildPendingAfterCurrent = false;
        s_Data->AutoReloadAfterBuild = true;
        BuildPluginProject();
    }

    // Clear flag on failure
    if (s_Data->AutoReloadAfterBuild && s_Data->BuildState == BuildStatus::Failed) {
        s_Data->AutoReloadAfterBuild = false;
    }

    // Path 2: DLL timestamp changed (external rebuild)
    if (s_Data->AutoReloadAfterBuild || s_Data->BuildState == BuildStatus::Building)
        return;

    if (!s_Data->DLLHandle || s_Data->DLLPath.empty()) return;

    auto srcPath = std::filesystem::path(s_Data->DLLPath);
    if (!std::filesystem::exists(srcPath)) return;

    auto currentTime = std::filesystem::last_write_time(srcPath);
    if (currentTime != s_Data->LastWriteTime) {
        VE_ENGINE_INFO("Plugin DLL changed (external), hot-reloading...");
        std::lock_guard<std::mutex> lock(s_Data->ReloadMutex);
        UnloadPluginDLL();
        LoadPluginDLL(s_Data->DLLPath);
        s_Data->ReloadedThisFrame = true;
    }
}

// ── File Watcher ────────────────────────────────────────────────────

void PluginEngine::SetupFileWatcher() {
    if (!s_Data) return;

    auto projPath = std::filesystem::absolute(s_Data->PluginSourcePath).string();
    if (!std::filesystem::exists(projPath))
        std::filesystem::create_directories(projPath);

    s_Data->PluginFileWatcher.Init(projPath, 0.5f);
    s_Data->PluginFileWatcher.SetCallback([](const std::vector<FileEvent>& events) {
        if (!s_Data) return;

        bool relevant = false;
        for (auto& e : events) {
            auto ext = std::filesystem::path(e.FilePath).extension().string();
            if (ext != ".cpp" && ext != ".h") continue;
            auto fname = std::filesystem::path(e.FilePath).filename().string();
            if (fname == "PluginRegistry.gen.cpp") continue;

            relevant = true;
            if (e.EventType == FileEvent::Type::Created || e.EventType == FileEvent::Type::Deleted)
                s_Data->SourceFilesAdded = true;
        }

        if (relevant) {
            s_Data->SourceChangeTimer = PluginEngineData::DebounceDelay;
            VE_ENGINE_INFO("Plugin source change detected, will build in {0}ms...",
                (int)(PluginEngineData::DebounceDelay * 1000));
        }
    });

    s_Data->FileWatcherInitialized = true;
    VE_ENGINE_INFO("Plugin file watcher started on: {0}", projPath);
}

void PluginEngine::UpdateFileWatcher(float dt) {
    if (!s_Data || !s_Data->FileWatcherInitialized) return;

    s_Data->PluginFileWatcher.Update(dt);

    if (s_Data->SourceChangeTimer > 0.0f) {
        s_Data->SourceChangeTimer -= dt;
        if (s_Data->SourceChangeTimer <= 0.0f) {
            s_Data->SourceChangeTimer = -1.0f;

            if (s_Data->SourceFilesAdded)
                s_Data->NeedsConfigure = true;
            s_Data->SourceFilesAdded = false;

            if (IsBuildInProgress()) {
                s_Data->BuildPendingAfterCurrent = true;
                VE_ENGINE_INFO("Plugin build already in progress, queuing...");
            } else {
                s_Data->AutoReloadAfterBuild = true;
                BuildPluginProject();
            }
        }
    }
}

void PluginEngine::RequestBuildAndReload() {
    if (!s_Data || IsBuildInProgress()) return;
    s_Data->AutoReloadAfterBuild = true;
    BuildPluginProject();
    VE_ENGINE_INFO("Manual plugin build+reload triggered");
}

// ── Configuration ───────────────────────────────────────────────────

void PluginEngine::SetPluginSourcePath(const std::string& path) {
    if (s_Data) s_Data->PluginSourcePath = path;
}

void PluginEngine::SetEngineIncludePath(const std::string& path) {
    if (s_Data) s_Data->EngineIncludePath = path;
}

void PluginEngine::SetImGuiSourcePath(const std::string& path) {
    if (s_Data) s_Data->ImGuiSourcePath = path;
}

bool PluginEngine::HasPluginFiles() {
    if (!s_Data) return false;
    auto projPath = std::filesystem::path(s_Data->PluginSourcePath);
    if (!std::filesystem::exists(projPath)) return false;
    for (auto& entry : std::filesystem::directory_iterator(projPath)) {
        if (entry.is_regular_file() && entry.path().extension() == ".cpp"
            && entry.path().filename().string() != "PluginRegistry.gen.cpp")
            return true;
    }
    return false;
}

std::vector<std::string> PluginEngine::GetLoadedPluginNames() {
    if (!s_Data) return {};
    return s_Data->PluginNames;
}

// ── Registry + CMakeLists Generation ────────────────────────────────

static void RegeneratePluginRegistry() {
    if (!s_Data) return;

    auto projPath = std::filesystem::path(s_Data->PluginSourcePath);
    if (!std::filesystem::exists(projPath)) return;

    std::vector<std::string> pluginNames;
    std::vector<std::string> cppFiles;
    std::regex registerRegex(R"(REGISTER_PLUGIN\(\s*(\w+)\s*\))");

    for (auto& entry : std::filesystem::directory_iterator(projPath)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension().string() != ".cpp") continue;
        if (entry.path().filename().string() == "PluginRegistry.gen.cpp") continue;

        cppFiles.push_back(entry.path().filename().string());

        std::ifstream fin(entry.path());
        std::string content((std::istreambuf_iterator<char>(fin)),
                             std::istreambuf_iterator<char>());
        fin.close();

        std::sregex_iterator it(content.begin(), content.end(), registerRegex);
        std::sregex_iterator end;
        for (; it != end; ++it)
            pluginNames.push_back((*it)[1].str());
    }

    auto buildDir = std::filesystem::absolute("PluginBuild");
    std::filesystem::create_directories(buildDir);

    auto absProjPath = std::filesystem::absolute(projPath);
    std::string srcDir = absProjPath.string();
    std::replace(srcDir.begin(), srcDir.end(), '\\', '/');

    // Generate PluginRegistry.gen.cpp
    {
        auto registryPath = buildDir / "PluginRegistry.gen.cpp";
        std::ofstream fout(registryPath);
        fout << "// Auto-generated by VibeEngine — do not edit manually\n";
        fout << "#include <VibeEngine/Editor/EditorPlugin.h>\n";
        fout << "#include <imgui.h>\n\n";

        for (auto& name : pluginNames)
            fout << "extern VE::EditorPlugin* Create_" << name << "();\n";

        fout << "\nstatic PluginEntry s_Plugins[] = {\n";
        for (size_t i = 0; i < pluginNames.size(); i++) {
            fout << "    { \"" << pluginNames[i] << "\", Create_" << pluginNames[i] << " }";
            if (i + 1 < pluginNames.size()) fout << ",";
            fout << "\n";
        }
        if (pluginNames.empty())
            fout << "    { nullptr, nullptr }\n";
        fout << "};\n\n";

        fout << "extern \"C\" VE_PLUGIN_API PluginEntry* GetPluginEntries(int* count) {\n";
        fout << "    *count = " << pluginNames.size() << ";\n";
        fout << "    return s_Plugins;\n";
        fout << "}\n\n";

        fout << "// Static member definition (DLL has its own copy)\n";
        fout << "VE::PluginViewport VE::EditorPlugin::Viewport = {};\n\n";

        fout << "extern \"C\" VE_PLUGIN_API void SetPluginContext(void* ctx) {\n";
        fout << "    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx));\n";
        fout << "}\n\n";

        fout << "extern \"C\" VE_PLUGIN_API void SetPluginViewport(float x, float y, float w, float h) {\n";
        fout << "    VE::EditorPlugin::Viewport = { x, y, w, h };\n";
        fout << "}\n";
        fout.close();
        VE_ENGINE_INFO("Generated PluginRegistry.gen.cpp with {0} plugins", pluginNames.size());
    }

    // Generate CMakeLists.txt
    {
        auto cmakePath = buildDir / "CMakeLists.txt";
        std::ofstream fout(cmakePath);
        fout << "# Auto-generated by VibeEngine — do not edit manually\n";
        fout << "cmake_minimum_required(VERSION 3.20)\n";
        fout << "project(EditorPlugins LANGUAGES CXX)\n";
        fout << "set(CMAKE_CXX_STANDARD 17)\n\n";

        // ImGui source path
        std::string imguiSrc = s_Data->ImGuiSourcePath;
        std::replace(imguiSrc.begin(), imguiSrc.end(), '\\', '/');

        fout << "add_library(EditorPlugins SHARED\n";
        for (auto& f : cppFiles)
            fout << "    \"" << srcDir << "/" << f << "\"\n";
        fout << "    \"${CMAKE_SOURCE_DIR}/PluginRegistry.gen.cpp\"\n";
        // ImGui core source files
        fout << "    \"" << imguiSrc << "/imgui.cpp\"\n";
        fout << "    \"" << imguiSrc << "/imgui_draw.cpp\"\n";
        fout << "    \"" << imguiSrc << "/imgui_tables.cpp\"\n";
        fout << "    \"" << imguiSrc << "/imgui_widgets.cpp\"\n";
        fout << ")\n\n";

        fout << "target_include_directories(EditorPlugins PRIVATE\n";
        if (!s_Data->EngineIncludePath.empty()) {
            std::string incPath = s_Data->EngineIncludePath;
            std::replace(incPath.begin(), incPath.end(), '\\', '/');
            fout << "    \"" << incPath << "\"\n";
        }
        fout << "    \"" << imguiSrc << "\"\n";
        fout << ")\n\n";

        fout << "target_compile_definitions(EditorPlugins PRIVATE VE_PLUGIN_DLL)\n";
        fout.close();
        VE_ENGINE_INFO("Generated plugin CMakeLists.txt with {0} files", cppFiles.size());
    }
}

// ── Build System ────────────────────────────────────────────────────

bool PluginEngine::BuildPluginProject() {
    if (!s_Data) return false;
    if (s_Data->BuildState == BuildStatus::Building) return false;

    auto projPath = std::filesystem::absolute(s_Data->PluginSourcePath).string();
    if (!std::filesystem::exists(projPath)) {
        VE_ENGINE_ERROR("Plugin source not found: {0}", projPath);
        return false;
    }

    s_Data->BuildState = BuildStatus::Building;
    {
        std::lock_guard<std::mutex> lock(s_Data->BuildMutex);
        s_Data->BuildOutput.clear();
    }

    if (s_Data->BuildThread.joinable())
        s_Data->BuildThread.join();

    // Always regenerate registry (ensures new/removed plugins are captured)
    RegeneratePluginRegistry();

    std::string cmakeBin = "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\Common7\\IDE\\CommonExtensions\\Microsoft\\CMake\\CMake\\bin\\cmake.exe";
    std::string buildDir = std::filesystem::absolute("PluginBuild").string();
    bool needsConfigure = s_Data->NeedsConfigure || !std::filesystem::exists(buildDir + "/out");

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

        if (needsConfigure) {
            std::string configCmd = "\"" + cmakeBin + "\" -B \"" + buildDir + "/out\" -S \"" + buildDir + "\" -G \"Visual Studio 17 2022\" -A x64";
            output += "=== Configuring ===\n";
            if (runCmd(configCmd) != 0) {
                output += "\n=== Configure FAILED ===\n";
                {
                    std::lock_guard<std::mutex> lock(s_Data->BuildMutex);
                    s_Data->BuildOutput = output;
                }
                s_Data->BuildState = PluginEngine::BuildStatus::Failed;
                return;
            }
            s_Data->NeedsConfigure = false;
        } else {
            output += "=== Skipping configure (incremental) ===\n";
        }

        std::string buildCmd = "\"" + cmakeBin + "\" --build \"" + buildDir + "/out\" --config Debug";
        output += "\n=== Building ===\n";
        if (runCmd(buildCmd) != 0) {
            output += "\n=== Build FAILED ===\n";
            {
                std::lock_guard<std::mutex> lock(s_Data->BuildMutex);
                s_Data->BuildOutput = output;
            }
            s_Data->BuildState = PluginEngine::BuildStatus::Failed;
            return;
        }

        output += "\n=== Build SUCCESS ===\n";
        std::string dllPath = buildDir + "/out/Debug/EditorPlugins.dll";
        {
            std::lock_guard<std::mutex> lock(s_Data->BuildMutex);
            s_Data->BuildOutput = output;
            s_Data->BuildDLLOutputPath = dllPath;
        }
        s_Data->BuildState = PluginEngine::BuildStatus::Success;
    });

    return true;
}

bool PluginEngine::BuildPluginProjectSync() {
    if (!s_Data) return false;

    auto projPath = std::filesystem::absolute(s_Data->PluginSourcePath).string();
    if (!std::filesystem::exists(projPath)) return false;
    if (!HasPluginFiles()) return false;

    RegeneratePluginRegistry();

    std::string cmakeBin = "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\Common7\\IDE\\CommonExtensions\\Microsoft\\CMake\\CMake\\bin\\cmake.exe";
    std::string buildDir = std::filesystem::absolute("PluginBuild").string();
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

    VE_ENGINE_INFO("Building plugin project (sync)...");

    bool needsConfigure = s_Data->NeedsConfigure || !std::filesystem::exists(buildDir + "/out");
    if (needsConfigure) {
        std::string configCmd = "\"" + cmakeBin + "\" -B \"" + buildDir + "/out\" -S \"" + buildDir + "\" -G \"Visual Studio 17 2022\" -A x64";
        if (runCmd(configCmd) != 0) {
            VE_ENGINE_ERROR("Plugin configure failed");
            s_Data->BuildOutput = output;
            s_Data->BuildState = BuildStatus::Failed;
            return false;
        }
        s_Data->NeedsConfigure = false;
    }

    std::string buildCmd = "\"" + cmakeBin + "\" --build \"" + buildDir + "/out\" --config Debug";
    if (runCmd(buildCmd) != 0) {
        VE_ENGINE_ERROR("Plugin build failed");
        s_Data->BuildOutput = output;
        s_Data->BuildState = BuildStatus::Failed;
        return false;
    }

    s_Data->BuildOutput = output;
    s_Data->BuildState = BuildStatus::Success;

    std::string dllPath = buildDir + "/out/Debug/EditorPlugins.dll";
    s_Data->BuildDLLOutputPath = dllPath;
    if (std::filesystem::exists(dllPath)) {
        LoadPluginDLL(dllPath);
        VE_ENGINE_INFO("Plugin DLL auto-loaded: {0}", dllPath);
        return true;
    }
    return false;
}

bool PluginEngine::IsBuildInProgress() {
    return s_Data && s_Data->BuildState == BuildStatus::Building;
}

const std::string& PluginEngine::GetBuildOutput() {
    static std::string empty;
    if (!s_Data) return empty;
    std::lock_guard<std::mutex> lock(s_Data->BuildMutex);
    return s_Data->BuildOutput;
}

PluginEngine::BuildStatus PluginEngine::GetBuildStatus() {
    return s_Data ? s_Data->BuildState.load() : BuildStatus::Idle;
}

} // namespace VE
