/*
 * ScriptGlue — Implements the ScriptAPI function pointers that bridge
 * engine internals to script DLLs.
 *
 * Each function reads/writes engine state via the ScriptEngine's active scene
 * and the Application singleton.
 */
#include "VibeEngine/Scripting/NativeScript.h"
#include "VibeEngine/Scripting/ScriptEngine.h"
#include "VibeEngine/Scene/Scene.h"
#include "VibeEngine/Scene/Entity.h"
#include "VibeEngine/Scene/Components.h"
#include "VibeEngine/Core/Log.h"
#include "VibeEngine/Core/Application.h"
#include "VibeEngine/Core/Window.h"
#include "VibeEngine/Audio/AudioEngine.h"
#include "VibeEngine/Input/Input.h"
#include "VibeEngine/Input/InputAction.h"

namespace VE {

// ── Logging ─────────────────────────────────────────────────────────

static void Glue_Log_Info(const char* msg) {
    VE_INFO("[Script] {0}", msg);
}

static void Glue_Log_Warn(const char* msg) {
    VE_WARN("[Script] {0}", msg);
}

static void Glue_Log_Error(const char* msg) {
    VE_ERROR("[Script] {0}", msg);
}

// ── Input (keyboard / mouse) ────────────────────────────────────────

static bool Glue_Input_IsKeyDown(int keyCode) {
    return Input::IsKeyDown(keyCode);
}

static bool Glue_Input_IsKeyPressed(int keyCode) {
    return Input::IsKeyPressed(keyCode);
}

static bool Glue_Input_IsKeyReleased(int keyCode) {
    return Input::IsKeyReleased(keyCode);
}

static bool Glue_Input_IsMouseButtonDown(int button) {
    return Input::IsMouseButtonDown(button);
}

static void Glue_Input_GetMousePosition(float* x, float* y) {
    auto pos = Input::GetMousePosition();
    *x = pos.x;
    *y = pos.y;
}

static void Glue_Input_GetMouseDelta(float* x, float* y) {
    auto delta = Input::GetMouseDelta();
    *x = delta.x;
    *y = delta.y;
}

static float Glue_Input_GetScrollDelta() {
    return Input::GetScrollDelta();
}

// ── Input (gamepad) ─────────────────────────────────────────────────

static bool Glue_Input_IsGamepadConnected(int gamepadID) {
    return Input::IsGamepadConnected(gamepadID);
}

static bool Glue_Input_IsGamepadButtonDown(int button, int gamepadID) {
    return Input::IsGamepadButtonDown(static_cast<GamepadButton>(button), gamepadID);
}

static bool Glue_Input_IsGamepadButtonPressed(int button, int gamepadID) {
    return Input::IsGamepadButtonPressed(static_cast<GamepadButton>(button), gamepadID);
}

static float Glue_Input_GetGamepadAxis(int axis, int gamepadID) {
    return Input::GetGamepadAxis(static_cast<GamepadAxis>(axis), gamepadID);
}

// ── Input (action system) ───────────────────────────────────────────

static float Glue_Input_GetActionValue(const char* path) {
    return InputActions::GetValue(path ? path : "");
}

static bool Glue_Input_IsActionPressed(const char* path) {
    return InputActions::IsPressed(path ? path : "");
}

static bool Glue_Input_IsActionDown(const char* path) {
    return InputActions::IsDown(path ? path : "");
}

// ── Time ────────────────────────────────────────────────────────────

static float Glue_Time_GetDeltaTime() {
    auto* app = Application::GetInstance();
    return app ? app->GetDeltaTime() : 0.0f;
}

// ── Entity ──────────────────────────────────────────────────────────

static void Glue_Entity_GetTransform(uint64_t entityID, ScriptTransform* out) {
    Scene* scene = ScriptEngine::GetActiveScene();
    if (!scene) return;

    auto& reg = scene->GetRegistry();
    auto view = reg.view<IDComponent, TransformComponent>();
    for (auto entity : view) {
        auto& id = view.get<IDComponent>(entity);
        if (static_cast<uint64_t>(id.ID) == entityID) {
            auto& tc = view.get<TransformComponent>(entity);
            out->Position[0] = tc.Position[0];
            out->Position[1] = tc.Position[1];
            out->Position[2] = tc.Position[2];
            out->Rotation[0] = tc.Rotation[0];
            out->Rotation[1] = tc.Rotation[1];
            out->Rotation[2] = tc.Rotation[2];
            out->Scale[0] = tc.Scale[0];
            out->Scale[1] = tc.Scale[1];
            out->Scale[2] = tc.Scale[2];
            return;
        }
    }
}

static void Glue_Entity_SetTransform(uint64_t entityID, const ScriptTransform* in) {
    Scene* scene = ScriptEngine::GetActiveScene();
    if (!scene) return;

    auto& reg = scene->GetRegistry();
    auto view = reg.view<IDComponent, TransformComponent>();
    for (auto entity : view) {
        auto& id = view.get<IDComponent>(entity);
        if (static_cast<uint64_t>(id.ID) == entityID) {
            auto& tc = view.get<TransformComponent>(entity);
            tc.Position[0] = in->Position[0];
            tc.Position[1] = in->Position[1];
            tc.Position[2] = in->Position[2];
            tc.Rotation[0] = in->Rotation[0];
            tc.Rotation[1] = in->Rotation[1];
            tc.Rotation[2] = in->Rotation[2];
            tc.Scale[0] = in->Scale[0];
            tc.Scale[1] = in->Scale[1];
            tc.Scale[2] = in->Scale[2];
            return;
        }
    }
}

static uint64_t Glue_Entity_FindByName(const char* name) {
    Scene* scene = ScriptEngine::GetActiveScene();
    if (!scene) return 0;

    auto& reg = scene->GetRegistry();
    auto view = reg.view<IDComponent, TagComponent>();
    for (auto entity : view) {
        auto& tag = view.get<TagComponent>(entity);
        if (tag.Tag == name) {
            auto& id = view.get<IDComponent>(entity);
            return static_cast<uint64_t>(id.ID);
        }
    }
    return 0;
}

// ── Audio ───────────────────────────────────────────────────────────

static uint32_t Glue_Audio_Play(const char* clipPath, float volume, float pitch, bool loop) {
    return AudioEngine::Play(clipPath ? clipPath : "", volume, pitch, loop);
}

static void Glue_Audio_Stop(uint32_t handle) {
    AudioEngine::Stop(handle);
}

static void Glue_Audio_SetVolume(uint32_t handle, float volume) {
    AudioEngine::SetVolume(handle, volume);
}

static void Glue_Audio_SetMasterVolume(float volume) {
    AudioEngine::SetMasterVolume(volume);
}

// ── Init ────────────────────────────────────────────────────────────

void InitScriptGlue(ScriptAPI& api) {
    api.Log_Info                 = Glue_Log_Info;
    api.Log_Warn                 = Glue_Log_Warn;
    api.Log_Error                = Glue_Log_Error;
    api.Input_IsKeyDown          = Glue_Input_IsKeyDown;
    api.Input_IsKeyPressed       = Glue_Input_IsKeyPressed;
    api.Input_IsKeyReleased      = Glue_Input_IsKeyReleased;
    api.Input_IsMouseButtonDown  = Glue_Input_IsMouseButtonDown;
    api.Input_GetMousePosition   = Glue_Input_GetMousePosition;
    api.Input_GetMouseDelta      = Glue_Input_GetMouseDelta;
    api.Input_GetScrollDelta     = Glue_Input_GetScrollDelta;
    api.Input_IsGamepadConnected     = Glue_Input_IsGamepadConnected;
    api.Input_IsGamepadButtonDown    = Glue_Input_IsGamepadButtonDown;
    api.Input_IsGamepadButtonPressed = Glue_Input_IsGamepadButtonPressed;
    api.Input_GetGamepadAxis         = Glue_Input_GetGamepadAxis;
    api.Input_GetActionValue     = Glue_Input_GetActionValue;
    api.Input_IsActionPressed    = Glue_Input_IsActionPressed;
    api.Input_IsActionDown       = Glue_Input_IsActionDown;
    api.Time_GetDeltaTime        = Glue_Time_GetDeltaTime;
    api.Entity_GetTransform      = Glue_Entity_GetTransform;
    api.Entity_SetTransform      = Glue_Entity_SetTransform;
    api.Entity_FindByName        = Glue_Entity_FindByName;
    api.Audio_Play               = Glue_Audio_Play;
    api.Audio_Stop               = Glue_Audio_Stop;
    api.Audio_SetVolume          = Glue_Audio_SetVolume;
    api.Audio_SetMasterVolume    = Glue_Audio_SetMasterVolume;
}

} // namespace VE
