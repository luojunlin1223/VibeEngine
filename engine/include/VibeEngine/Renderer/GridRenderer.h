/*
 * GridRenderer — Draws an infinite grid using GL_LINES with depth testing.
 *
 * Unlike ImGui-based gizmo lines, this grid participates in the depth
 * buffer so 3D objects properly occlude it.
 */
#pragma once

#include <glm/glm.hpp>

namespace VE {

class GridRenderer {
public:
    static void Init();
    static void Shutdown();

    // Draw grid on XZ plane (3D) or XY plane (2D).
    // Call inside a 3D render pass with depth testing enabled.
    static void DrawGrid(const glm::mat4& viewProjection,
                          const glm::vec3& cameraPos,
                          float baseSpacing = 1.0f,
                          bool is2D = false);
};

} // namespace VE
