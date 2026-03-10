#include "VibeEngine/Renderer/GizmoRenderer.h"
#include "VibeEngine/Scene/Components.h"

#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

namespace VE {

// ── Static state ───────────────────────────────────────────────────

static glm::mat4 s_VP(1.0f);
static glm::mat4 s_InvVP(1.0f);
static float s_VpX = 0, s_VpY = 0, s_VpW = 1280, s_VpH = 720;
static CameraMode s_CameraMode = CameraMode::Perspective3D;

static constexpr float kGizmoArmLength  = 0.4f;
static constexpr float kGizmoArrowSize  = 0.06f;

// ── Helpers ────────────────────────────────────────────────────────

static ImVec2 WorldToScreen(const glm::vec3& world) {
    glm::vec4 clip = s_VP * glm::vec4(world, 1.0f);
    if (clip.w == 0.0f) return { -1, -1 };
    glm::vec3 ndc = glm::vec3(clip) / clip.w;

    float sx = s_VpX + (ndc.x * 0.5f + 0.5f) * s_VpW;
    float sy = s_VpY + (1.0f - (ndc.y * 0.5f + 0.5f)) * s_VpH;
    return { sx, sy };
}

static void DrawLineWorld(ImDrawList* dl, const glm::vec3& a, const glm::vec3& b,
                          ImU32 color, float thickness = 1.0f) {
    ImVec2 sa = WorldToScreen(a);
    ImVec2 sb = WorldToScreen(b);
    dl->AddLine(sa, sb, color, thickness);
}

static float PointToSegmentDistanceSS(ImVec2 p, ImVec2 a, ImVec2 b) {
    float dx = b.x - a.x, dy = b.y - a.y;
    float lenSq = dx * dx + dy * dy;
    if (lenSq < 1e-6f) {
        float ex = p.x - a.x, ey = p.y - a.y;
        return std::sqrt(ex * ex + ey * ey);
    }
    float t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / lenSq;
    t = std::max(0.0f, std::min(1.0f, t));
    float cx = a.x + t * dx - p.x;
    float cy = a.y + t * dy - p.y;
    return std::sqrt(cx * cx + cy * cy);
}

// ── API ────────────────────────────────────────────────────────────

void GizmoRenderer::BeginScene(const glm::mat4& viewProjection,
                               float viewportX, float viewportY,
                               float viewportW, float viewportH,
                               CameraMode cameraMode) {
    s_VP         = viewProjection;
    s_InvVP      = glm::inverse(viewProjection);
    s_VpX        = viewportX;
    s_VpY        = viewportY;
    s_VpW        = viewportW;
    s_VpH        = viewportH;
    s_CameraMode = cameraMode;
}

glm::vec2 GizmoRenderer::ScreenToWorld(float screenX, float screenY) {
    float ndcX = ((screenX - s_VpX) / s_VpW) * 2.0f - 1.0f;
    float ndcY = 1.0f - ((screenY - s_VpY) / s_VpH) * 2.0f;

    glm::vec4 world = s_InvVP * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
    return { world.x / world.w, world.y / world.w };
}

GizmoAxis GizmoRenderer::HitTestTranslationGizmo(const glm::vec3& entityPos,
                                                   float screenX, float screenY,
                                                   float pixelThreshold) {
    ImVec2 mouse = { screenX, screenY };
    ImVec2 originSS = WorldToScreen(entityPos);
    ImVec2 xEndSS   = WorldToScreen(entityPos + glm::vec3(kGizmoArmLength, 0, 0));
    ImVec2 yEndSS   = WorldToScreen(entityPos + glm::vec3(0, kGizmoArmLength, 0));
    ImVec2 zEndSS   = WorldToScreen(entityPos + glm::vec3(0, 0, kGizmoArmLength));

    float distX = PointToSegmentDistanceSS(mouse, originSS, xEndSS);
    float distY = PointToSegmentDistanceSS(mouse, originSS, yEndSS);
    float distZ = PointToSegmentDistanceSS(mouse, originSS, zEndSS);

    // Find the closest axis within threshold
    float best = pixelThreshold;
    GizmoAxis result = GizmoAxis::None;

    if (distX < best) { best = distX; result = GizmoAxis::X; }
    if (distY < best) { best = distY; result = GizmoAxis::Y; }
    if (distZ < best) { best = distZ; result = GizmoAxis::Z; }
    return result;
}

float GizmoRenderer::ProjectMouseOntoAxis(GizmoAxis axis,
                                            const glm::vec3& entityPos,
                                            float screenX, float screenY) {
    // Project the mouse onto the axis line in screen space, then find
    // the world-space position along that axis.
    glm::vec3 axisDir(0);
    int component = 0;
    if (axis == GizmoAxis::X) { axisDir = glm::vec3(1, 0, 0); component = 0; }
    if (axis == GizmoAxis::Y) { axisDir = glm::vec3(0, 1, 0); component = 1; }
    if (axis == GizmoAxis::Z) { axisDir = glm::vec3(0, 0, 1); component = 2; }

    // Two points on the axis in screen space
    ImVec2 aSS = WorldToScreen(entityPos);
    ImVec2 bSS = WorldToScreen(entityPos + axisDir);

    float dx = bSS.x - aSS.x, dy = bSS.y - aSS.y;
    float lenSq = dx * dx + dy * dy;
    if (lenSq < 1e-6f) return entityPos[component];

    // t parameter: how far along the screen-space axis is the mouse?
    float t = ((screenX - aSS.x) * dx + (screenY - aSS.y) * dy) / lenSq;

    // Map t back to world: entityPos + t * axisDir (1 world unit in screen)
    return entityPos[component] + t;
}

void GizmoRenderer::DrawGrid(float gridSize, float spacing) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    float half = gridSize * 0.5f;

    ImU32 gridColor  = IM_COL32(80, 80, 80, 100);
    ImU32 axisXColor = IM_COL32(130, 40, 40, 180);
    ImU32 axisYColor = IM_COL32(40, 130, 40, 180);
    ImU32 axisZColor = IM_COL32(40, 40, 130, 180);

    if (s_CameraMode == CameraMode::Orthographic2D) {
        // XY plane grid (2D mode)
        for (float i = -half; i <= half; i += spacing) {
            float eps = spacing * 0.01f;
            ImU32 col = (i > -eps && i < eps) ? axisYColor : gridColor;
            DrawLineWorld(dl, { i, -half, 0 }, { i, half, 0 }, col);
            col = (i > -eps && i < eps) ? axisXColor : gridColor;
            DrawLineWorld(dl, { -half, i, 0 }, { half, i, 0 }, col);
        }
    } else {
        // XZ plane grid (3D mode)
        for (float i = -half; i <= half; i += spacing) {
            float eps = spacing * 0.01f;
            ImU32 col = (i > -eps && i < eps) ? axisZColor : gridColor;
            DrawLineWorld(dl, { i, 0, -half }, { i, 0, half }, col);
            col = (i > -eps && i < eps) ? axisXColor : gridColor;
            DrawLineWorld(dl, { -half, 0, i }, { half, 0, i }, col);
        }
    }
}

void GizmoRenderer::DrawTranslationGizmo(Entity entity, GizmoAxis highlightAxis) {
    if (!entity || !entity.HasComponent<TransformComponent>()) return;

    auto& tc = entity.GetComponent<TransformComponent>();
    glm::vec3 pos(tc.Position[0], tc.Position[1], tc.Position[2]);

    ImDrawList* dl = ImGui::GetForegroundDrawList();

    ImU32 red      = IM_COL32(230, 50, 50, 255);
    ImU32 green    = IM_COL32(50, 200, 50, 255);
    ImU32 blue     = IM_COL32(80, 80, 240, 255);
    ImU32 highlight = IM_COL32(255, 255, 50, 255);

    float thick      = 2.5f;
    float thickHover = 4.0f;

    // X axis (red)
    ImU32 xCol   = (highlightAxis == GizmoAxis::X) ? highlight : red;
    float xThick = (highlightAxis == GizmoAxis::X) ? thickHover : thick;
    glm::vec3 xEnd = pos + glm::vec3(kGizmoArmLength, 0, 0);
    DrawLineWorld(dl, pos, xEnd, xCol, xThick);
    DrawLineWorld(dl, xEnd, xEnd + glm::vec3(-kGizmoArrowSize, kGizmoArrowSize * 0.5f, 0), xCol, xThick);
    DrawLineWorld(dl, xEnd, xEnd + glm::vec3(-kGizmoArrowSize, -kGizmoArrowSize * 0.5f, 0), xCol, xThick);

    // Y axis (green)
    ImU32 yCol   = (highlightAxis == GizmoAxis::Y) ? highlight : green;
    float yThick = (highlightAxis == GizmoAxis::Y) ? thickHover : thick;
    glm::vec3 yEnd = pos + glm::vec3(0, kGizmoArmLength, 0);
    DrawLineWorld(dl, pos, yEnd, yCol, yThick);
    DrawLineWorld(dl, yEnd, yEnd + glm::vec3(kGizmoArrowSize * 0.5f, -kGizmoArrowSize, 0), yCol, yThick);
    DrawLineWorld(dl, yEnd, yEnd + glm::vec3(-kGizmoArrowSize * 0.5f, -kGizmoArrowSize, 0), yCol, yThick);

    // Z axis (blue)
    ImU32 zCol   = (highlightAxis == GizmoAxis::Z) ? highlight : blue;
    float zThick = (highlightAxis == GizmoAxis::Z) ? thickHover : thick;
    glm::vec3 zEnd = pos + glm::vec3(0, 0, kGizmoArmLength);
    DrawLineWorld(dl, pos, zEnd, zCol, zThick);
    DrawLineWorld(dl, zEnd, zEnd + glm::vec3(0, kGizmoArrowSize * 0.5f, -kGizmoArrowSize), zCol, zThick);
    DrawLineWorld(dl, zEnd, zEnd + glm::vec3(0, -kGizmoArrowSize * 0.5f, -kGizmoArrowSize), zCol, zThick);
}

void GizmoRenderer::DrawWireframeBox(Entity entity) {
    if (!entity || !entity.HasComponent<TransformComponent>()) return;

    auto& tc = entity.GetComponent<TransformComponent>();
    glm::vec3 pos(tc.Position[0], tc.Position[1], tc.Position[2]);
    glm::vec3 scl(tc.Scale[0], tc.Scale[1], tc.Scale[2]);

    float hx = 0.5f * scl.x;
    float hy = 0.5f * scl.y;
    float hz = 0.5f * scl.z;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImU32 wireColor = IM_COL32(255, 150, 0, 255);

    // 8 corners of the AABB
    glm::vec3 corners[8] = {
        pos + glm::vec3(-hx, -hy, -hz), pos + glm::vec3( hx, -hy, -hz),
        pos + glm::vec3( hx,  hy, -hz), pos + glm::vec3(-hx,  hy, -hz),
        pos + glm::vec3(-hx, -hy,  hz), pos + glm::vec3( hx, -hy,  hz),
        pos + glm::vec3( hx,  hy,  hz), pos + glm::vec3(-hx,  hy,  hz),
    };

    // 12 edges
    int edges[][2] = {
        {0,1},{1,2},{2,3},{3,0}, // back face
        {4,5},{5,6},{6,7},{7,4}, // front face
        {0,4},{1,5},{2,6},{3,7}, // connecting edges
    };
    for (auto& e : edges)
        DrawLineWorld(dl, corners[e[0]], corners[e[1]], wireColor, 1.5f);
}

} // namespace VE
