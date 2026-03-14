/*
 * NativeScript — Base class for C++ native scripts loaded from DLLs.
 *
 * Scripts inherit from NativeScript and override OnCreate/OnUpdate/OnDestroy.
 * The engine communicates with script DLLs via a function pointer table (ScriptAPI)
 * to avoid link-time dependencies. Scripts use REGISTER_SCRIPT() macro and the
 * auto-generated ScriptRegistry.gen.cpp exports GetScriptEntries() / SetScriptAPI().
 */
#pragma once
#include <cstdint>
#include <cstddef>

namespace VE {

struct ScriptTransform {
    float Position[3] = { 0.0f, 0.0f, 0.0f };
    float Rotation[3] = { 0.0f, 0.0f, 0.0f };
    float Scale[3]    = { 1.0f, 1.0f, 1.0f };
};

// Function pointer table passed from engine to script DLL
struct ScriptAPI {
    // Logging
    void (*Log_Info)(const char* msg)            = nullptr;
    void (*Log_Warn)(const char* msg)            = nullptr;
    void (*Log_Error)(const char* msg)           = nullptr;

    // Input: keyboard
    bool (*Input_IsKeyDown)(int keyCode)         = nullptr;
    bool (*Input_IsKeyPressed)(int keyCode)      = nullptr;
    bool (*Input_IsKeyReleased)(int keyCode)     = nullptr;

    // Input: mouse
    bool (*Input_IsMouseButtonDown)(int button)  = nullptr;
    void (*Input_GetMousePosition)(float* x, float* y) = nullptr;
    void (*Input_GetMouseDelta)(float* x, float* y)    = nullptr;
    float (*Input_GetScrollDelta)()              = nullptr;

    // Input: gamepad
    bool  (*Input_IsGamepadConnected)(int gamepadID)               = nullptr;
    bool  (*Input_IsGamepadButtonDown)(int button, int gamepadID)  = nullptr;
    bool  (*Input_IsGamepadButtonPressed)(int button, int gamepadID) = nullptr;
    float (*Input_GetGamepadAxis)(int axis, int gamepadID)         = nullptr;

    // Input: action system
    float (*Input_GetActionValue)(const char* path)   = nullptr;
    bool  (*Input_IsActionPressed)(const char* path)  = nullptr;
    bool  (*Input_IsActionDown)(const char* path)     = nullptr;

    // Time
    float (*Time_GetDeltaTime)()                 = nullptr;

    // Entity
    void (*Entity_GetTransform)(uint64_t entityID, ScriptTransform* out) = nullptr;
    void (*Entity_SetTransform)(uint64_t entityID, const ScriptTransform* in) = nullptr;
    uint64_t (*Entity_FindByName)(const char* name) = nullptr;

    // Audio
    uint32_t (*Audio_Play)(const char* clipPath, float volume, float pitch, bool loop) = nullptr;
    void (*Audio_Stop)(uint32_t soundHandle)         = nullptr;
    void (*Audio_SetVolume)(uint32_t soundHandle, float volume) = nullptr;
    void (*Audio_SetMasterVolume)(float volume)       = nullptr;
};

// ── Property reflection ─────────────────────────────────────────────

enum class ScriptPropertyType { Float, Int, Bool, Vec3, String };

struct ScriptPropertyInfo {
    const char* Name;
    ScriptPropertyType Type;
    size_t Offset;  // offsetof within the script instance
};

class NativeScript {
public:
    virtual ~NativeScript() = default;

    virtual void OnCreate() {}
    virtual void OnUpdate(float deltaTime) {}
    virtual void OnDestroy() {}

    // Property reflection — override via VE_PROPERTIES macro
    virtual const ScriptPropertyInfo* GetProperties(int& count) const { count = 0; return nullptr; }

    uint64_t GetEntityID() const { return m_EntityID; }

    // Called by registry to set API from DLL side
    static void SetAPI(const ScriptAPI* api) { API = api; }

protected:
    // Convenience: access the API table from scripts
    static const ScriptAPI* API;

    ScriptTransform GetTransform() const {
        ScriptTransform t;
        if (API && API->Entity_GetTransform)
            API->Entity_GetTransform(m_EntityID, &t);
        return t;
    }

    void SetTransform(const ScriptTransform& t) {
        if (API && API->Entity_SetTransform)
            API->Entity_SetTransform(m_EntityID, &t);
    }

private:
    uint64_t m_EntityID = 0;
    friend class ScriptEngine;
    friend class Scene;
};

} // namespace VE

// DLL export macros
#ifdef VE_SCRIPT_DLL
  #ifdef _WIN32
    #define VE_SCRIPT_API __declspec(dllexport)
  #else
    #define VE_SCRIPT_API __attribute__((visibility("default")))
  #endif
#else
  #define VE_SCRIPT_API
#endif

// Script registration macro — generates a factory function with external linkage
#define REGISTER_SCRIPT(ClassName) \
    VE::NativeScript* Create_##ClassName() { return new ClassName(); }

// Entry in the script registry table exported by the DLL
struct ScriptEntry {
    const char* Name;
    VE::NativeScript* (*CreateFunc)();
};

// ── Property exposure macros ────────────────────────────────────────

#define VE_PROPERTIES(ClassName, ...) \
    const VE::ScriptPropertyInfo* GetProperties(int& count) const override { \
        static VE::ScriptPropertyInfo props[] = { __VA_ARGS__ }; \
        count = sizeof(props) / sizeof(props[0]); \
        return props; \
    }

#define VE_FLOAT(ClassName, name) \
    { #name, VE::ScriptPropertyType::Float, offsetof(ClassName, name) }

#define VE_INT(ClassName, name) \
    { #name, VE::ScriptPropertyType::Int, offsetof(ClassName, name) }

#define VE_BOOL(ClassName, name) \
    { #name, VE::ScriptPropertyType::Bool, offsetof(ClassName, name) }

// DLL must export (via auto-generated ScriptRegistry.gen.cpp):
//   extern "C" VE_SCRIPT_API ScriptEntry* GetScriptEntries(int* count);
//   extern "C" VE_SCRIPT_API void SetScriptAPI(const VE::ScriptAPI* api);
