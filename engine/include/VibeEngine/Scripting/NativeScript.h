/*
 * NativeScript — Base class for C++ native scripts loaded from DLLs.
 *
 * Scripts inherit from NativeScript and override OnCreate/OnUpdate/OnDestroy.
 * The engine communicates with script DLLs via a function pointer table (ScriptAPI)
 * to avoid link-time dependencies. Scripts use REGISTER_SCRIPT() macro and export
 * GetScriptEntries() / SetScriptAPI() functions.
 */
#pragma once
#include <cstdint>

namespace VE {

struct ScriptTransform {
    float Position[3] = { 0.0f, 0.0f, 0.0f };
    float Rotation[3] = { 0.0f, 0.0f, 0.0f };
    float Scale[3]    = { 1.0f, 1.0f, 1.0f };
};

// Function pointer table passed from engine to script DLL
struct ScriptAPI {
    void (*Log_Info)(const char* msg)            = nullptr;
    void (*Log_Warn)(const char* msg)            = nullptr;
    void (*Log_Error)(const char* msg)           = nullptr;
    bool (*Input_IsKeyDown)(int keyCode)         = nullptr;
    bool (*Input_IsMouseButtonDown)(int button)  = nullptr;
    void (*Input_GetMousePosition)(float* x, float* y) = nullptr;
    float (*Time_GetDeltaTime)()                 = nullptr;
    void (*Entity_GetTransform)(uint64_t entityID, ScriptTransform* out) = nullptr;
    void (*Entity_SetTransform)(uint64_t entityID, const ScriptTransform* in) = nullptr;
    uint64_t (*Entity_FindByName)(const char* name) = nullptr;
};

class NativeScript {
public:
    virtual ~NativeScript() = default;

    virtual void OnCreate() {}
    virtual void OnUpdate(float deltaTime) {}
    virtual void OnDestroy() {}

    uint64_t GetEntityID() const { return m_EntityID; }

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

// Script registration macro — generates a factory function
#define REGISTER_SCRIPT(ClassName) \
    static VE::NativeScript* Create_##ClassName() { return new ClassName(); }

// Entry in the script registry table exported by the DLL
struct ScriptEntry {
    const char* Name;
    VE::NativeScript* (*CreateFunc)();
};

// DLL must export:
//   extern "C" VE_SCRIPT_API ScriptEntry* GetScriptEntries(int* count);
//   extern "C" VE_SCRIPT_API void SetScriptAPI(const VE::ScriptAPI* api);
