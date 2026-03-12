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

#include <GLFW/glfw3.h>

namespace VE {

// ── API implementations ─────────────────────────────────────────────

static void Glue_Log_Info(const char* msg) {
    VE_INFO("[Script] {0}", msg);
}

static void Glue_Log_Warn(const char* msg) {
    VE_WARN("[Script] {0}", msg);
}

static void Glue_Log_Error(const char* msg) {
    VE_ERROR("[Script] {0}", msg);
}

static bool Glue_Input_IsKeyDown(int keyCode) {
    auto* app = Application::GetInstance();
    if (!app) return false;
    GLFWwindow* window = app->GetWindow().GetNativeWindow();
    return glfwGetKey(window, keyCode) == GLFW_PRESS;
}

static bool Glue_Input_IsMouseButtonDown(int button) {
    auto* app = Application::GetInstance();
    if (!app) return false;
    GLFWwindow* window = app->GetWindow().GetNativeWindow();
    return glfwGetMouseButton(window, button) == GLFW_PRESS;
}

static void Glue_Input_GetMousePosition(float* x, float* y) {
    auto* app = Application::GetInstance();
    if (!app) { *x = 0; *y = 0; return; }
    double dx, dy;
    glfwGetCursorPos(app->GetWindow().GetNativeWindow(), &dx, &dy);
    *x = static_cast<float>(dx);
    *y = static_cast<float>(dy);
}

static float Glue_Time_GetDeltaTime() {
    auto* app = Application::GetInstance();
    return app ? app->GetDeltaTime() : 0.0f;
}

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

// ── Init ────────────────────────────────────────────────────────────

void InitScriptGlue(ScriptAPI& api) {
    api.Log_Info             = Glue_Log_Info;
    api.Log_Warn             = Glue_Log_Warn;
    api.Log_Error            = Glue_Log_Error;
    api.Input_IsKeyDown      = Glue_Input_IsKeyDown;
    api.Input_IsMouseButtonDown = Glue_Input_IsMouseButtonDown;
    api.Input_GetMousePosition  = Glue_Input_GetMousePosition;
    api.Time_GetDeltaTime    = Glue_Time_GetDeltaTime;
    api.Entity_GetTransform  = Glue_Entity_GetTransform;
    api.Entity_SetTransform  = Glue_Entity_SetTransform;
    api.Entity_FindByName    = Glue_Entity_FindByName;
}

} // namespace VE
