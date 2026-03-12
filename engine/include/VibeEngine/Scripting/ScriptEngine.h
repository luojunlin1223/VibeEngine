/*
 * ScriptEngine — Loads C++ script DLLs and manages script instances.
 *
 * Supports hot-reload: watches the DLL file for changes, copies it to
 * a temp path (so the user can rebuild), reloads, and recreates all
 * script instances.
 */
#pragma once
#include <string>
#include <vector>
#include <memory>

namespace VE {

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
};

} // namespace VE
