/*
 * ScriptGlue — Implements the ScriptAPI function pointers that bridge
 * engine internals to script DLLs.
 */
#include "VibeEngine/Scripting/NativeScript.h"
#include "VibeEngine/Scripting/ScriptEngine.h"
#include "VibeEngine/Scene/Scene.h"
#include "VibeEngine/Scene/Entity.h"
#include "VibeEngine/Scene/Components.h"
#include "VibeEngine/Scene/MeshLibrary.h"
#include "VibeEngine/Core/Log.h"
#include "VibeEngine/Core/Application.h"
#include "VibeEngine/Core/Window.h"
#include "VibeEngine/Audio/AudioEngine.h"
#include "VibeEngine/Input/Input.h"
#include "VibeEngine/Input/InputAction.h"
#include "VibeEngine/Physics/PhysicsWorld.h"
#include "VibeEngine/Renderer/Material.h"

namespace VE {

// ── Helper: find entt::entity by UUID ───────────────────────────────

static entt::entity FindEntityByUUID(Scene* scene, uint64_t entityID) {
    if (!scene) return entt::null;
    auto& reg = scene->GetRegistry();
    auto view = reg.view<IDComponent>();
    for (auto entity : view) {
        if (static_cast<uint64_t>(view.get<IDComponent>(entity).ID) == entityID)
            return entity;
    }
    return entt::null;
}

// ── Logging ─────────────────────────────────────────────────────────

static void Glue_Log_Info(const char* msg)  { VE_INFO("[Script] {0}", msg); }
static void Glue_Log_Warn(const char* msg)  { VE_WARN("[Script] {0}", msg); }
static void Glue_Log_Error(const char* msg) { VE_ERROR("[Script] {0}", msg); }

// ── Input (keyboard / mouse) ────────────────────────────────────────

static bool Glue_Input_IsKeyDown(int keyCode)    { return Input::IsKeyDown(keyCode); }
static bool Glue_Input_IsKeyPressed(int keyCode)  { return Input::IsKeyPressed(keyCode); }
static bool Glue_Input_IsKeyReleased(int keyCode) { return Input::IsKeyReleased(keyCode); }

static bool Glue_Input_IsMouseButtonDown(int button) { return Input::IsMouseButtonDown(button); }

static void Glue_Input_GetMousePosition(float* x, float* y) {
    auto pos = Input::GetMousePosition(); *x = pos.x; *y = pos.y;
}
static void Glue_Input_GetMouseDelta(float* x, float* y) {
    auto d = Input::GetMouseDelta(); *x = d.x; *y = d.y;
}
static float Glue_Input_GetScrollDelta() { return Input::GetScrollDelta(); }

// ── Input (gamepad) ─────────────────────────────────────────────────

static bool Glue_Input_IsGamepadConnected(int id) { return Input::IsGamepadConnected(id); }
static bool Glue_Input_IsGamepadButtonDown(int b, int id) {
    return Input::IsGamepadButtonDown(static_cast<GamepadButton>(b), id);
}
static bool Glue_Input_IsGamepadButtonPressed(int b, int id) {
    return Input::IsGamepadButtonPressed(static_cast<GamepadButton>(b), id);
}
static float Glue_Input_GetGamepadAxis(int a, int id) {
    return Input::GetGamepadAxis(static_cast<GamepadAxis>(a), id);
}

// ── Input (action system) ───────────────────────────────────────────

static float Glue_Input_GetActionValue(const char* p)  { return InputActions::GetValue(p ? p : ""); }
static bool  Glue_Input_IsActionPressed(const char* p)  { return InputActions::IsPressed(p ? p : ""); }
static bool  Glue_Input_IsActionDown(const char* p)     { return InputActions::IsDown(p ? p : ""); }

// ── Time ────────────────────────────────────────────────────────────

static float Glue_Time_GetDeltaTime() {
    auto* app = Application::GetInstance();
    return app ? app->GetDeltaTime() : 0.0f;
}

// ── Entity: transform ───────────────────────────────────────────────

static void Glue_Entity_GetTransform(uint64_t entityID, ScriptTransform* out) {
    Scene* scene = ScriptEngine::GetActiveScene();
    auto e = FindEntityByUUID(scene, entityID);
    if (e == entt::null) return;
    auto& tc = scene->GetRegistry().get<TransformComponent>(e);
    for (int i = 0; i < 3; i++) {
        out->Position[i] = tc.Position[i];
        out->Rotation[i] = tc.Rotation[i];
        out->Scale[i]    = tc.Scale[i];
    }
}

static void Glue_Entity_SetTransform(uint64_t entityID, const ScriptTransform* in) {
    Scene* scene = ScriptEngine::GetActiveScene();
    auto e = FindEntityByUUID(scene, entityID);
    if (e == entt::null) return;
    auto& tc = scene->GetRegistry().get<TransformComponent>(e);
    for (int i = 0; i < 3; i++) {
        tc.Position[i] = in->Position[i];
        tc.Rotation[i] = in->Rotation[i];
        tc.Scale[i]    = in->Scale[i];
    }
}

static uint64_t Glue_Entity_FindByName(const char* name) {
    Scene* scene = ScriptEngine::GetActiveScene();
    if (!scene) return 0;
    auto& reg = scene->GetRegistry();
    auto view = reg.view<IDComponent, TagComponent>();
    for (auto entity : view) {
        if (view.get<TagComponent>(entity).Tag == name)
            return static_cast<uint64_t>(view.get<IDComponent>(entity).ID);
    }
    return 0;
}

// ── Entity: lifecycle ───────────────────────────────────────────────

static uint64_t Glue_Entity_Create(const char* name) {
    Scene* scene = ScriptEngine::GetActiveScene();
    if (!scene) return 0;
    Entity e = scene->CreateEntity(name ? name : "Entity");

    // Add default MeshRenderer with cube
    auto& mr = e.AddComponent<MeshRendererComponent>();
    mr.Mesh = MeshLibrary::GetCube();
    mr.Mat = MaterialLibrary::Get("Lit");

    return static_cast<uint64_t>(e.GetComponent<IDComponent>().ID);
}

static void Glue_Entity_Destroy(uint64_t entityID) {
    Scene* scene = ScriptEngine::GetActiveScene();
    auto e = FindEntityByUUID(scene, entityID);
    if (e == entt::null) return;
    Entity entity(e, scene);
    scene->DestroyEntity(entity);
}

static void Glue_Entity_SetActive(uint64_t entityID, bool active) {
    Scene* scene = ScriptEngine::GetActiveScene();
    auto e = FindEntityByUUID(scene, entityID);
    if (e == entt::null) return;
    auto* tag = scene->GetRegistry().try_get<TagComponent>(e);
    if (tag) tag->Active = active;
}

// ── Entity: component access ────────────────────────────────────────

static void Glue_Entity_SetColor(uint64_t entityID, float r, float g, float b, float a) {
    Scene* scene = ScriptEngine::GetActiveScene();
    auto e = FindEntityByUUID(scene, entityID);
    if (e == entt::null) return;
    auto* mr = scene->GetRegistry().try_get<MeshRendererComponent>(e);
    if (mr) mr->Color = { r, g, b, a };
}

// ── Physics: rigidbody control ──────────────────────────────────────

static void Glue_Physics_AddForce(uint64_t entityID, float fx, float fy, float fz) {
    Scene* scene = ScriptEngine::GetActiveScene();
    auto e = FindEntityByUUID(scene, entityID);
    if (e == entt::null) return;
    auto* rb = scene->GetRegistry().try_get<RigidbodyComponent>(e);
    if (!rb || rb->_JoltBodyID == 0xFFFFFFFF) return;
    auto* pw = scene->GetPhysicsWorld();
    if (pw) pw->AddForce(rb->_JoltBodyID, { fx, fy, fz });
}

static void Glue_Physics_AddImpulse(uint64_t entityID, float fx, float fy, float fz) {
    Scene* scene = ScriptEngine::GetActiveScene();
    auto e = FindEntityByUUID(scene, entityID);
    if (e == entt::null) return;
    auto* rb = scene->GetRegistry().try_get<RigidbodyComponent>(e);
    if (!rb || rb->_JoltBodyID == 0xFFFFFFFF) return;
    auto* pw = scene->GetPhysicsWorld();
    if (pw) pw->AddImpulse(rb->_JoltBodyID, { fx, fy, fz });
}

static void Glue_Physics_SetVelocity(uint64_t entityID, float vx, float vy, float vz) {
    Scene* scene = ScriptEngine::GetActiveScene();
    auto e = FindEntityByUUID(scene, entityID);
    if (e == entt::null) return;
    auto* rb = scene->GetRegistry().try_get<RigidbodyComponent>(e);
    if (!rb || rb->_JoltBodyID == 0xFFFFFFFF) return;
    auto* pw = scene->GetPhysicsWorld();
    if (pw) pw->SetLinearVelocity(rb->_JoltBodyID, { vx, vy, vz });
}

static void Glue_Physics_GetVelocity(uint64_t entityID, float* vx, float* vy, float* vz) {
    Scene* scene = ScriptEngine::GetActiveScene();
    auto e = FindEntityByUUID(scene, entityID);
    if (e == entt::null) { *vx = *vy = *vz = 0; return; }
    auto* rb = scene->GetRegistry().try_get<RigidbodyComponent>(e);
    if (!rb || rb->_JoltBodyID == 0xFFFFFFFF) { *vx = *vy = *vz = 0; return; }
    auto* pw = scene->GetPhysicsWorld();
    if (!pw) { *vx = *vy = *vz = 0; return; }
    auto v = pw->GetLinearVelocity(rb->_JoltBodyID);
    *vx = v.x; *vy = v.y; *vz = v.z;
}

static void Glue_Physics_SetAngularVelocity(uint64_t entityID, float vx, float vy, float vz) {
    Scene* scene = ScriptEngine::GetActiveScene();
    auto e = FindEntityByUUID(scene, entityID);
    if (e == entt::null) return;
    auto* rb = scene->GetRegistry().try_get<RigidbodyComponent>(e);
    if (!rb || rb->_JoltBodyID == 0xFFFFFFFF) return;
    auto* pw = scene->GetPhysicsWorld();
    if (pw) pw->SetAngularVelocity(rb->_JoltBodyID, { vx, vy, vz });
}

// ── Physics: queries ────────────────────────────────────────────────

static bool Glue_Physics_Raycast(float ox, float oy, float oz,
                                  float dx, float dy, float dz,
                                  float maxDist, ScriptRaycastHit* hit) {
    Scene* scene = ScriptEngine::GetActiveScene();
    if (!scene) return false;
    auto* pw = scene->GetPhysicsWorld();
    if (!pw) return false;

    RaycastHit rh;
    if (pw->Raycast({ ox, oy, oz }, { dx, dy, dz }, maxDist, rh)) {
        hit->Point[0] = rh.Point.x; hit->Point[1] = rh.Point.y; hit->Point[2] = rh.Point.z;
        hit->Normal[0] = rh.Normal.x; hit->Normal[1] = rh.Normal.y; hit->Normal[2] = rh.Normal.z;
        hit->Distance = rh.Distance;

        // Map body ID to entity UUID
        auto entity = scene->FindEntityByBodyID(rh.BodyID);
        if (entity != entt::null) {
            auto* id = scene->GetRegistry().try_get<IDComponent>(entity);
            hit->EntityID = id ? static_cast<uint64_t>(id->ID) : 0;
        }
        return true;
    }
    return false;
}

// ── Scene ───────────────────────────────────────────────────────────

static void Glue_Scene_LoadScene(const char* path) {
    Scene* scene = ScriptEngine::GetActiveScene();
    if (scene && path)
        scene->RequestLoadScene(path);
}

// ── Audio ───────────────────────────────────────────────────────────

static uint32_t Glue_Audio_Play(const char* clipPath, float volume, float pitch, bool loop) {
    return AudioEngine::Play(clipPath ? clipPath : "", volume, pitch, loop);
}
static void Glue_Audio_Stop(uint32_t handle)              { AudioEngine::Stop(handle); }
static void Glue_Audio_SetVolume(uint32_t handle, float v) { AudioEngine::SetVolume(handle, v); }
static void Glue_Audio_SetMasterVolume(float v)            { AudioEngine::SetMasterVolume(v); }

// ── Init ────────────────────────────────────────────────────────────

void InitScriptGlue(ScriptAPI& api) {
    // Logging
    api.Log_Info                 = Glue_Log_Info;
    api.Log_Warn                 = Glue_Log_Warn;
    api.Log_Error                = Glue_Log_Error;

    // Input
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

    // Time
    api.Time_GetDeltaTime        = Glue_Time_GetDeltaTime;

    // Entity
    api.Entity_GetTransform      = Glue_Entity_GetTransform;
    api.Entity_SetTransform      = Glue_Entity_SetTransform;
    api.Entity_FindByName        = Glue_Entity_FindByName;
    api.Entity_Create            = Glue_Entity_Create;
    api.Entity_Destroy           = Glue_Entity_Destroy;
    api.Entity_SetActive         = Glue_Entity_SetActive;
    api.Entity_SetColor          = Glue_Entity_SetColor;

    // Physics
    api.Physics_AddForce         = Glue_Physics_AddForce;
    api.Physics_AddImpulse       = Glue_Physics_AddImpulse;
    api.Physics_SetVelocity      = Glue_Physics_SetVelocity;
    api.Physics_GetVelocity      = Glue_Physics_GetVelocity;
    api.Physics_SetAngularVelocity = Glue_Physics_SetAngularVelocity;
    api.Physics_Raycast          = Glue_Physics_Raycast;

    // Scene
    api.Scene_LoadScene          = Glue_Scene_LoadScene;

    // Audio
    api.Audio_Play               = Glue_Audio_Play;
    api.Audio_Stop               = Glue_Audio_Stop;
    api.Audio_SetVolume          = Glue_Audio_SetVolume;
    api.Audio_SetMasterVolume    = Glue_Audio_SetMasterVolume;
}

} // namespace VE
