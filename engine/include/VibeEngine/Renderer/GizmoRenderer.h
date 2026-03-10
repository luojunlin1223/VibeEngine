/*
 * GizmoRenderer — Draws Unity-style gizmos using ImGui's ImDrawList.
 *
 * Backend-agnostic: works with both OpenGL and Vulkan since all
 * drawing goes through ImGui's rendering pipeline.
 *
 * World-space coordinates are projected to screen-space using the
 * editor camera's view-projection matrix.
 *
 * Renders:
 *   - Grid lines in the XY plane
 *   - Wireframe bounding box around selected entity
 *   - Translation gizmo: 3 colored axis arrows (R=X, G=Y, B=Z)
 */
#pragma once

#include "VibeEngine/Scene/Entity.h"
#include <glm/glm.hpp>

namespace VE {

class GizmoRenderer {
public:
    // Call once per frame before any Draw calls
    static void BeginScene(const glm::mat4& viewProjection,
                           float viewportX, float viewportY,
                           float viewportW, float viewportH);

    static void DrawGrid(float gridSize = 10.0f, float spacing = 1.0f);
    static void DrawTranslationGizmo(Entity entity);
    static void DrawWireframeBox(Entity entity);
};

} // namespace VE
