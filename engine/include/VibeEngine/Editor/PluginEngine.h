/*
 * PluginEngine — Manages hot-reloadable editor plugin DLLs.
 *
 * Mirrors ScriptEngine but for editor UI plugins.  Plugins get full ImGui
 * access and run in edit mode (no Play required).
 */
#pragma once
#include <string>
#include <vector>

namespace VE {

class PluginEngine {
public:
    static void Init();
    static void Shutdown();

    // DLL management
    static bool LoadPluginDLL(const std::string& path);
    static void UnloadPluginDLL();
    static bool IsLoaded();

    // Render all loaded plugin UIs — call inside OnImGuiRender
    static void RenderAllPluginUI();

    // Set viewport bounds so plugins can draw overlays inside it
    static void SetViewportBounds(float x, float y, float w, float h);

    // Hot-reload — call once per frame
    static void CheckForReload();

    // File watcher + auto-build-on-save
    static void SetupFileWatcher();
    static void UpdateFileWatcher(float dt);
    static void RequestBuildAndReload();

    // Build system
    static bool BuildPluginProject();       // async
    static bool BuildPluginProjectSync();   // blocking, auto-loads DLL
    static bool IsBuildInProgress();
    static const std::string& GetBuildOutput();
    static bool HasPluginFiles();

    // Configuration (call before SetupFileWatcher / Build)
    static void SetPluginSourcePath(const std::string& path);
    static void SetEngineIncludePath(const std::string& path);
    static void SetImGuiSourcePath(const std::string& path);

    enum class BuildStatus { Idle, Building, Success, Failed };
    static BuildStatus GetBuildStatus();

    static std::vector<std::string> GetLoadedPluginNames();
};

} // namespace VE
