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

namespace VE {

enum class GizmoAxis { None, X, Y, Z };

class GizmoRenderer {
public:
    static void BeginScene(const glm::mat4& viewProjection,
                           float viewportX, float viewportY,
                           float viewportW, float viewportH,
                           CameraMode cameraMode = CameraMode::Perspective3D);

    static void DrawGrid(float gridSize = 20.0f, float spacing = 1.0f);
    static void DrawTranslationGizmo(Entity entity, GizmoAxis highlightAxis = GizmoAxis::None);
    static void DrawWireframeBox(Entity entity);

    // Screen ↔ World conversion
    static glm::vec2 ScreenToWorld(float screenX, float screenY);

    // Hit-test gizmo axes in screen space (works for 2D and 3D).
    // pixelThreshold is the hit distance in pixels.
    static GizmoAxis HitTestTranslationGizmo(const glm::vec3& entityPos,
                                              float screenX, float screenY,
                                              float pixelThreshold = 12.0f);

    // Project a world position along a gizmo axis using screen-space mouse delta.
    // Returns the new world position along the given axis.
    static float ProjectMouseOntoAxis(GizmoAxis axis,
                                       const glm::vec3& entityPos,
                                       float screenX, float screenY);
};

} // namespace VE
