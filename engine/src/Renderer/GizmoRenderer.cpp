#include "VibeEngine/Renderer/GizmoRenderer.h"
#include "VibeEngine/Scene/Components.h"

#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>

namespace VE {

// ── Static state ───────────────────────────────────────────────────

static glm::mat4 s_VP(1.0f);
static float s_VpX = 0, s_VpY = 0, s_VpW = 1280, s_VpH = 720;

// ── Helpers ────────────────────────────────────────────────────────

// Project a world-space point to screen-space (pixel coordinates)
static ImVec2 WorldToScreen(const glm::vec3& world) {
    glm::vec4 clip = s_VP * glm::vec4(world, 1.0f);
    if (clip.w == 0.0f) return { -1, -1 };
    glm::vec3 ndc = glm::vec3(clip) / clip.w;

    float sx = s_VpX + (ndc.x * 0.5f + 0.5f) * s_VpW;
    float sy = s_VpY + (1.0f - (ndc.y * 0.5f + 0.5f)) * s_VpH; // flip Y for screen
    return { sx, sy };
}

static void DrawLineWorld(ImDrawList* dl, const glm::vec3& a, const glm::vec3& b,
                          ImU32 color, float thickness = 1.0f) {
    ImVec2 sa = WorldToScreen(a);
    ImVec2 sb = WorldToScreen(b);
    dl->AddLine(sa, sb, color, thickness);
}

// ── API ────────────────────────────────────────────────────────────

void GizmoRenderer::BeginScene(const glm::mat4& viewProjection,
                               float viewportX, float viewportY,
                               float viewportW, float viewportH) {
    s_VP  = viewProjection;
    s_VpX = viewportX;
    s_VpY = viewportY;
    s_VpW = viewportW;
    s_VpH = viewportH;
}

void GizmoRenderer::DrawGrid(float gridSize, float spacing) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    float half = gridSize * 0.5f;

    ImU32 gridColor  = IM_COL32(80, 80, 80, 100);
    ImU32 axisXColor = IM_COL32(130, 40, 40, 180);
    ImU32 axisYColor = IM_COL32(40, 130, 40, 180);

    for (float i = -half; i <= half; i += spacing) {
        float eps = spacing * 0.01f;

        // Vertical lines (parallel to Y axis)
        ImU32 col = (i > -eps && i < eps) ? axisYColor : gridColor;
        DrawLineWorld(dl, { i, -half, 0 }, { i, half, 0 }, col);

        // Horizontal lines (parallel to X axis)
        col = (i > -eps && i < eps) ? axisXColor : gridColor;
        DrawLineWorld(dl, { -half, i, 0 }, { half, i, 0 }, col);
    }
}

void GizmoRenderer::DrawTranslationGizmo(Entity entity) {
    if (!entity || !entity.HasComponent<TransformComponent>()) return;

    auto& tc = entity.GetComponent<TransformComponent>();
    glm::vec3 pos(tc.Position[0], tc.Position[1], tc.Position[2]);

    ImDrawList* dl = ImGui::GetForegroundDrawList();

    // Compute arm length in world units that looks reasonable on screen
    // Use a fixed screen-space length → convert back to world
    float armLen = 0.4f;

    ImU32 red   = IM_COL32(230, 50, 50, 255);
    ImU32 green = IM_COL32(50, 200, 50, 255);
    ImU32 blue  = IM_COL32(80, 80, 240, 255);

    float thick = 2.5f;
    float arrowSize = 0.06f;

    // X axis (red)
    glm::vec3 xEnd = pos + glm::vec3(armLen, 0, 0);
    DrawLineWorld(dl, pos, xEnd, red, thick);
    DrawLineWorld(dl, xEnd, xEnd + glm::vec3(-arrowSize, arrowSize * 0.5f, 0), red, thick);
    DrawLineWorld(dl, xEnd, xEnd + glm::vec3(-arrowSize, -arrowSize * 0.5f, 0), red, thick);

    // Y axis (green)
    glm::vec3 yEnd = pos + glm::vec3(0, armLen, 0);
    DrawLineWorld(dl, pos, yEnd, green, thick);
    DrawLineWorld(dl, yEnd, yEnd + glm::vec3(arrowSize * 0.5f, -arrowSize, 0), green, thick);
    DrawLineWorld(dl, yEnd, yEnd + glm::vec3(-arrowSize * 0.5f, -arrowSize, 0), green, thick);

    // Z axis (blue) — draw as a diagonal hint since we're 2D
    glm::vec3 zEnd = pos + glm::vec3(0, 0, armLen);
    DrawLineWorld(dl, pos, zEnd, blue, thick);
    DrawLineWorld(dl, zEnd, zEnd + glm::vec3(arrowSize * 0.5f, 0, -arrowSize), blue, thick);
    DrawLineWorld(dl, zEnd, zEnd + glm::vec3(-arrowSize * 0.5f, 0, -arrowSize), blue, thick);
}

void GizmoRenderer::DrawWireframeBox(Entity entity) {
    if (!entity || !entity.HasComponent<TransformComponent>()) return;

    auto& tc = entity.GetComponent<TransformComponent>();
    glm::vec3 pos(tc.Position[0], tc.Position[1], tc.Position[2]);
    glm::vec3 scl(tc.Scale[0], tc.Scale[1], tc.Scale[2]);

    float hx = 0.5f * scl.x;
    float hy = 0.5f * scl.y;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImU32 wireColor = IM_COL32(255, 150, 0, 255); // orange

    glm::vec3 bl = pos + glm::vec3(-hx, -hy, 0);
    glm::vec3 br = pos + glm::vec3( hx, -hy, 0);
    glm::vec3 tr = pos + glm::vec3( hx,  hy, 0);
    glm::vec3 tl = pos + glm::vec3(-hx,  hy, 0);

    DrawLineWorld(dl, bl, br, wireColor, 1.5f);
    DrawLineWorld(dl, br, tr, wireColor, 1.5f);
    DrawLineWorld(dl, tr, tl, wireColor, 1.5f);
    DrawLineWorld(dl, tl, bl, wireColor, 1.5f);
}

} // namespace VE
