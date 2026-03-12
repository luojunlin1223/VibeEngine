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

    // Clip all gizmo drawing to the viewport rectangle
    ImVec2 clipMin(viewportX, viewportY);
    ImVec2 clipMax(viewportX + viewportW, viewportY + viewportH);
    ImGui::GetBackgroundDrawList()->PushClipRect(clipMin, clipMax, true);
    ImGui::GetForegroundDrawList()->PushClipRect(clipMin, clipMax, true);
}

void GizmoRenderer::EndScene() {
    ImGui::GetBackgroundDrawList()->PopClipRect();
    ImGui::GetForegroundDrawList()->PopClipRect();
}

glm::vec2 GizmoRenderer::ScreenToWorld(float screenX, float screenY) {
    float ndcX = ((screenX - s_VpX) / s_VpW) * 2.0f - 1.0f;
    float ndcY = 1.0f - ((screenY - s_VpY) / s_VpH) * 2.0f;

    glm::vec4 world = s_InvVP * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
    return { world.x / world.w, world.y / world.w };
}

GizmoAxis GizmoRenderer::HitTestTranslationGizmo(const glm::vec3& entityPos,
                                                   float screenX, float screenY,
                                                   float pixelThreshold,
                                                   const glm::mat3& rotMatrix) {
    glm::vec3 axisX = rotMatrix * glm::vec3(1, 0, 0);
    glm::vec3 axisY = rotMatrix * glm::vec3(0, 1, 0);
    glm::vec3 axisZ = rotMatrix * glm::vec3(0, 0, 1);

    ImVec2 mouse = { screenX, screenY };
    ImVec2 originSS = WorldToScreen(entityPos);
    ImVec2 xEndSS   = WorldToScreen(entityPos + axisX * kGizmoArmLength);
    ImVec2 yEndSS   = WorldToScreen(entityPos + axisY * kGizmoArmLength);
    ImVec2 zEndSS   = WorldToScreen(entityPos + axisZ * kGizmoArmLength);

    float distX = PointToSegmentDistanceSS(mouse, originSS, xEndSS);
    float distY = PointToSegmentDistanceSS(mouse, originSS, yEndSS);
    float distZ = PointToSegmentDistanceSS(mouse, originSS, zEndSS);

    float best = pixelThreshold;
    GizmoAxis result = GizmoAxis::None;

    if (distX < best) { best = distX; result = GizmoAxis::X; }
    if (distY < best) { best = distY; result = GizmoAxis::Y; }
    if (distZ < best) { best = distZ; result = GizmoAxis::Z; }
    return result;
}

float GizmoRenderer::ProjectMouseOntoAxis(GizmoAxis axis,
                                            const glm::vec3& entityPos,
                                            float screenX, float screenY,
                                            const glm::mat3& rotMatrix) {
    // Get rotated axis direction
    glm::vec3 localDir(0);
    int component = 0;
    if (axis == GizmoAxis::X) { localDir = glm::vec3(1, 0, 0); component = 0; }
    if (axis == GizmoAxis::Y) { localDir = glm::vec3(0, 1, 0); component = 1; }
    if (axis == GizmoAxis::Z) { localDir = glm::vec3(0, 0, 1); component = 2; }

    glm::vec3 axisDir = rotMatrix * localDir;

    // Two points on the axis in screen space
    ImVec2 aSS = WorldToScreen(entityPos);
    ImVec2 bSS = WorldToScreen(entityPos + axisDir);

    float dx = bSS.x - aSS.x, dy = bSS.y - aSS.y;
    float lenSq = dx * dx + dy * dy;
    if (lenSq < 1e-6f) return entityPos[component];

    float t = ((screenX - aSS.x) * dx + (screenY - aSS.y) * dy) / lenSq;

    // Project t back along the rotated axis, but return the local component
    glm::vec3 worldOffset = axisDir * t;
    return entityPos[component] + worldOffset[component];
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

// Build rotation matrix from TransformComponent
static glm::mat3 GetRotationMatrix(const TransformComponent& tc) {
    glm::mat4 rot(1.0f);
    rot = glm::rotate(rot, glm::radians(tc.Rotation[0]), glm::vec3(1, 0, 0));
    rot = glm::rotate(rot, glm::radians(tc.Rotation[1]), glm::vec3(0, 1, 0));
    rot = glm::rotate(rot, glm::radians(tc.Rotation[2]), glm::vec3(0, 0, 1));
    return glm::mat3(rot);
}

void GizmoRenderer::DrawTranslationGizmo(Entity entity, GizmoAxis highlightAxis,
                                          const glm::mat4& worldMatrix) {
    if (!entity || !entity.HasComponent<TransformComponent>()) return;

    glm::vec3 pos = glm::vec3(worldMatrix[3]);
    glm::mat3 rot = glm::mat3(worldMatrix);
    // Normalize rotation axes (remove scale)
    rot[0] = glm::normalize(rot[0]);
    rot[1] = glm::normalize(rot[1]);
    rot[2] = glm::normalize(rot[2]);

    // Local axes rotated to world space
    glm::vec3 axisX = rot * glm::vec3(1, 0, 0);
    glm::vec3 axisY = rot * glm::vec3(0, 1, 0);
    glm::vec3 axisZ = rot * glm::vec3(0, 0, 1);

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
    glm::vec3 xEnd = pos + axisX * kGizmoArmLength;
    glm::vec3 xArrowPerp = axisY * (kGizmoArrowSize * 0.5f);
    DrawLineWorld(dl, pos, xEnd, xCol, xThick);
    DrawLineWorld(dl, xEnd, xEnd - axisX * kGizmoArrowSize + xArrowPerp, xCol, xThick);
    DrawLineWorld(dl, xEnd, xEnd - axisX * kGizmoArrowSize - xArrowPerp, xCol, xThick);

    // Y axis (green)
    ImU32 yCol   = (highlightAxis == GizmoAxis::Y) ? highlight : green;
    float yThick = (highlightAxis == GizmoAxis::Y) ? thickHover : thick;
    glm::vec3 yEnd = pos + axisY * kGizmoArmLength;
    glm::vec3 yArrowPerp = axisX * (kGizmoArrowSize * 0.5f);
    DrawLineWorld(dl, pos, yEnd, yCol, yThick);
    DrawLineWorld(dl, yEnd, yEnd - axisY * kGizmoArrowSize + yArrowPerp, yCol, yThick);
    DrawLineWorld(dl, yEnd, yEnd - axisY * kGizmoArrowSize - yArrowPerp, yCol, yThick);

    // Z axis (blue)
    ImU32 zCol   = (highlightAxis == GizmoAxis::Z) ? highlight : blue;
    float zThick = (highlightAxis == GizmoAxis::Z) ? thickHover : thick;
    glm::vec3 zEnd = pos + axisZ * kGizmoArmLength;
    glm::vec3 zArrowPerp = axisY * (kGizmoArrowSize * 0.5f);
    DrawLineWorld(dl, pos, zEnd, zCol, zThick);
    DrawLineWorld(dl, zEnd, zEnd - axisZ * kGizmoArrowSize + zArrowPerp, zCol, zThick);
    DrawLineWorld(dl, zEnd, zEnd - axisZ * kGizmoArrowSize - zArrowPerp, zCol, zThick);
}

void GizmoRenderer::DrawWireframeBox(const glm::mat4& worldMatrix) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    glm::mat4 model = worldMatrix;
    ImU32 wireColor = IM_COL32(255, 150, 0, 255);

    // 8 corners of a unit cube in local space, transformed to world space
    glm::vec3 local[8] = {
        { -0.5f, -0.5f, -0.5f }, {  0.5f, -0.5f, -0.5f },
        {  0.5f,  0.5f, -0.5f }, { -0.5f,  0.5f, -0.5f },
        { -0.5f, -0.5f,  0.5f }, {  0.5f, -0.5f,  0.5f },
        {  0.5f,  0.5f,  0.5f }, { -0.5f,  0.5f,  0.5f },
    };

    glm::vec3 corners[8];
    for (int i = 0; i < 8; i++)
        corners[i] = glm::vec3(model * glm::vec4(local[i], 1.0f));

    // 12 edges
    int edges[][2] = {
        {0,1},{1,2},{2,3},{3,0},
        {4,5},{5,6},{6,7},{7,4},
        {0,4},{1,5},{2,6},{3,7},
    };
    for (auto& e : edges)
        DrawLineWorld(dl, corners[e[0]], corners[e[1]], wireColor, 1.5f);
}

} // namespace VE
