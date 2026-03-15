/*
 * GizmoRenderer — Draws Unity-style gizmos using ImGui's ImDrawList.
 *
 * Backend-agnostic: works with both OpenGL and Vulkan since all
 * drawing goes through ImGui's rendering pipeline.
 *
 * Supports both 2D (XY plane) and 3D (XZ plane) grid modes.
 */
#pragma once

#include "VibeEngine/Scene/Entity.h"
#include "VibeEngine/Renderer/EditorCamera.h"
#include <glm/glm.hpp>

struct ImDrawList;

namespace VE {

enum class GizmoAxis { None, X, Y, Z };

class GizmoRenderer {
public:
    static void BeginScene(const glm::mat4& viewProjection,
                           float viewportX, float viewportY,
                           float viewportW, float viewportH,
                           CameraMode cameraMode = CameraMode::Perspective3D,
                           ImDrawList* drawList = nullptr);
    static void EndScene();

    static void DrawGrid(float gridSize = 20.0f, float spacing = 1.0f);
    static void DrawTranslationGizmo(Entity entity, GizmoAxis highlightAxis = GizmoAxis::None,
                                      const glm::mat4& worldMatrix = glm::mat4(1.0f));
    static void DrawWireframeBox(const glm::mat4& worldMatrix);
    static void DrawPointLightGizmo(const glm::vec3& position, float range,
                                     const glm::vec3& color = glm::vec3(1.0f));
    static void DrawSpotLightGizmo(const glm::vec3& position, const glm::vec3& direction,
                                    float range, float outerAngle,
                                    const glm::vec3& color = glm::vec3(1.0f));
    static void DrawCameraFrustum(const glm::mat4& worldTransform,
                                   int projType, float fov, float size,
                                   float nearClip, float farClip, float aspect);

    // Screen ↔ World conversion
    static glm::vec2 ScreenToWorld(float screenX, float screenY);

    // Hit-test gizmo axes in screen space (works for 2D and 3D).
    // pixelThreshold is the hit distance in pixels.
    static GizmoAxis HitTestTranslationGizmo(const glm::vec3& entityPos,
                                              float screenX, float screenY,
                                              float pixelThreshold = 12.0f,
                                              const glm::mat3& rotMatrix = glm::mat3(1.0f));

    static float ProjectMouseOntoAxis(GizmoAxis axis,
                                       const glm::vec3& entityPos,
                                       float screenX, float screenY,
                                       const glm::mat3& rotMatrix = glm::mat3(1.0f));
};

} // namespace VE
