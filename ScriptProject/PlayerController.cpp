/*
 * PlayerController — Example native script for VibeEngine.
 *
 * Moves the attached entity with WASD keys.
 * Demonstrates: OnCreate/OnUpdate lifecycle, input queries, transform access.
 */
#include <VibeEngine/Scripting/NativeScript.h>
using namespace VE;

static const ScriptAPI* API = nullptr;

class PlayerController : public NativeScript {
    float m_Speed = 5.0f;

public:
    void OnCreate() override {
        if (API && API->Log_Info)
            API->Log_Info("PlayerController created!");
    }

    void OnUpdate(float dt) override {
        if (!API) return;

        auto t = GetTransform();

        // WASD movement (GLFW key codes)
        if (API->Input_IsKeyDown(87))  // W
            t.Position[2] -= m_Speed * dt;
        if (API->Input_IsKeyDown(83))  // S
            t.Position[2] += m_Speed * dt;
        if (API->Input_IsKeyDown(65))  // A
            t.Position[0] -= m_Speed * dt;
        if (API->Input_IsKeyDown(68))  // D
            t.Position[0] += m_Speed * dt;
        if (API->Input_IsKeyDown(32))  // Space
            t.Position[1] += m_Speed * dt;
        if (API->Input_IsKeyDown(340)) // Left Shift
            t.Position[1] -= m_Speed * dt;

        SetTransform(t);
    }

    void OnDestroy() override {
        if (API && API->Log_Info)
            API->Log_Info("PlayerController destroyed!");
    }
};

REGISTER_SCRIPT(PlayerController)

// ── DLL exports ────────────────────────────────────────────────────

static ScriptEntry s_Scripts[] = {
    { "PlayerController", Create_PlayerController },
};

extern "C" VE_SCRIPT_API ScriptEntry* GetScriptEntries(int* count) {
    *count = sizeof(s_Scripts) / sizeof(s_Scripts[0]);
    return s_Scripts;
}

extern "C" VE_SCRIPT_API void SetScriptAPI(const VE::ScriptAPI* api) {
    API = api;
    // Also set the base class API so GetTransform/SetTransform work
    // (NativeScript::API is set by the engine, but we set our local copy too)
}
