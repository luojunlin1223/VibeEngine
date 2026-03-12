/*
 * ScriptEngine — Implementation of DLL loading, hot-reload, and instance management.
 *
 * Hot-reload flow:
 * 1. Each frame, compare DLL file's last-write-time against stored timestamp
 * 2. On change: destroy all instances → FreeLibrary → copy DLL to temp → LoadLibrary
 * 3. Re-query factory table → recreate instances → call OnCreate
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

} // namespace VE
