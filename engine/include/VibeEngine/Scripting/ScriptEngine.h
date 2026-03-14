/*
 * ScriptEngine — Loads C++ script DLLs and manages script instances.
 *
 * Supports hot-reload, script project management, auto-compile, and
 * new script generation with registry auto-generation.
 */
#pragma once
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <variant>
#include <glm/glm.hpp>

namespace VE {

// Set camera matrices for script ScreenToWorldRay / WorldToScreen.
// Call each frame before scripts run.
void SetScriptCameraMatrices(const glm::mat4& view, const glm::mat4& proj,
                              float viewportW, float viewportH);

class Scene;
class NativeScript;

class ScriptEngine {
public:
    static void Init();
    static void Shutdown();

    // DLL management
    static bool LoadScriptDLL(const std::string& path);
    static void UnloadScriptDLL();
    static bool IsLoaded();

    // Instance management
    static NativeScript* CreateInstance(const std::string& className);
    static void DestroyInstance(NativeScript* instance);

    // Hot-reload — call once per frame
    static void CheckForReload();
    static void ReloadScripts(Scene& scene);

    // Scene context
    static void SetActiveScene(Scene* scene);
    static Scene* GetActiveScene();

    // Info
    static std::string GetDLLPath();
    static std::vector<std::string> GetRegisteredClassNames();
    static bool WasReloadedThisFrame();

    // Property metadata — cached at DLL load time
    struct CachedProperty {
        std::string Name;
        int Type;      // ScriptPropertyType enum
        size_t Offset;
        // Default value
        float DefaultFloat = 0.0f;
        int DefaultInt = 0;
        bool DefaultBool = false;
    };
    static std::vector<CachedProperty> GetScriptProperties(const std::string& className);

    // Apply stored property values to a live instance, or read from instance to storage
    static void ApplyPropertiesToInstance(NativeScript* instance, const std::string& className,
        const std::unordered_map<std::string, std::variant<float, int, bool>>& values);
    static void ReadPropertiesFromInstance(NativeScript* instance, const std::string& className,
        std::unordered_map<std::string, std::variant<float, int, bool>>& values);

    // ── Script project management ───────────────────────────────────

    static void SetScriptProjectPath(const std::string& path);
    static std::string GetScriptProjectPath();
    static void SetEngineIncludePath(const std::string& path);

    // Create a new script .cpp file from template, regenerate registry + CMakeLists
    static bool CreateNewScript(const std::string& className);

    // Scan .cpp files for REGISTER_SCRIPT, regenerate ScriptRegistry.gen.cpp + CMakeLists.txt
    static void RegenerateScriptRegistry();

    // Scan ScriptProject .cpp files for REGISTER_SCRIPT and return class names (no DLL needed)
    static std::vector<std::string> ScanScriptClassNames();

    // Check if script project has any .cpp files
    static bool HasScriptFiles();

    // ── Auto-compile ────────────────────────────────────────────────

    static bool BuildScriptProject();           // async (background thread)
    static bool BuildScriptProjectSync();       // blocking — returns true on success, auto-loads DLL
    static bool IsBuildInProgress();
    static const std::string& GetBuildOutput();
    static const std::string& GetBuildDLLOutputPath();

    enum class BuildStatus { Idle, Building, Success, Failed };
    static BuildStatus GetBuildStatus();
};

} // namespace VE
