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
static ImDrawList* s_DrawList = nullptr;

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
    // Clip line against near plane (w > 0) to avoid projection artifacts
    glm::vec4 clipA = s_VP * glm::vec4(a, 1.0f);
    glm::vec4 clipB = s_VP * glm::vec4(b, 1.0f);

    const float nearW = 0.01f;
    bool behindA = clipA.w < nearW;
    bool behindB = clipB.w < nearW;

    if (behindA && behindB) return; // both behind camera

    if (behindA || behindB) {
        // Clip the behind-camera point to the near plane
        float t = (nearW - clipA.w) / (clipB.w - clipA.w);
        glm::vec4 clipMid = clipA + t * (clipB - clipA);
        if (behindA) clipA = clipMid;
        else         clipB = clipMid;
    }

    // Project to screen
    auto toScreen = [](const glm::vec4& clip) -> ImVec2 {
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        float sx = s_VpX + (ndc.x * 0.5f + 0.5f) * s_VpW;
        float sy = s_VpY + (1.0f - (ndc.y * 0.5f + 0.5f)) * s_VpH;
        return { sx, sy };
    };

    ImVec2 sa = toScreen(clipA);
    ImVec2 sb = toScreen(clipB);

    // Cull lines entirely outside viewport (with margin)
    float margin = 2000.0f;
    if (sa.x < s_VpX - margin && sb.x < s_VpX - margin) return;
    if (sa.y < s_VpY - margin && sb.y < s_VpY - margin) return;
    if (sa.x > s_VpX + s_VpW + margin && sb.x > s_VpX + s_VpW + margin) return;
    if (sa.y > s_VpY + s_VpH + margin && sb.y > s_VpY + s_VpH + margin) return;

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
                               CameraMode cameraMode,
                               ImDrawList* drawList) {
    s_VP         = viewProjection;
    s_InvVP      = glm::inverse(viewProjection);
    s_VpX        = viewportX;
    s_VpY        = viewportY;
    s_VpW        = viewportW;
    s_VpH        = viewportH;
    s_CameraMode = cameraMode;
    s_DrawList   = drawList ? drawList : ImGui::GetForegroundDrawList();

    // Clip all gizmo drawing to the viewport rectangle
    ImVec2 clipMin(viewportX, viewportY);
    ImVec2 clipMax(viewportX + viewportW, viewportY + viewportH);
    s_DrawList->PushClipRect(clipMin, clipMax, true);
}

void GizmoRenderer::EndScene() {
    s_DrawList->PopClipRect();
    s_DrawList = nullptr;
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

void GizmoRenderer::DrawGrid(float /*gridSize*/, float spacing) {
    ImDrawList* dl = s_DrawList;

    // Extract camera position from inverse VP
    glm::vec4 camPosH = s_InvVP * glm::vec4(0, 0, 0, 1);
    glm::vec3 camPos = glm::vec3(camPosH) / camPosH.w;

    ImU32 axisXColor = IM_COL32(130, 40, 40, 180);
    ImU32 axisYColor = IM_COL32(40, 130, 40, 180);
    ImU32 axisZColor = IM_COL32(40, 40, 130, 180);

    // Multi-layer grid: draw 3 layers with increasing spacing (1m, 10m, 100m).
    // Each layer covers a huge range but has fewer lines at wider spacing.
    // Lines use a very large extent (50000 units) so they appear infinite.
    const float kHugeExtent = 50000.0f;
    const float spacings[] = { spacing, spacing * 10.0f, spacing * 100.0f };
    const int   maxLines[] = { 80, 40, 20 };
    const int   baseAlpha[] = { 50, 70, 90 };

    bool is2D = (s_CameraMode == CameraMode::Orthographic2D);

    for (int layer = 0; layer < 3; layer++) {
        float sp = spacings[layer];
        int count = maxLines[layer];
        int alpha = baseAlpha[layer];

        if (is2D) {
            float cx = std::floor(camPos.x / sp) * sp;
            float cy = std::floor(camPos.y / sp) * sp;

            for (int i = -count; i <= count; i++) {
                float gx = cx + i * sp;
                float gy = cy + i * sp;

                // Fade at edges of this layer
                float fx = 1.0f - std::abs((float)i) / (float)count;
                fx = fx * fx; // smoother falloff
                int a = static_cast<int>(fx * alpha);
                if (a <= 0) continue;

                bool originX = std::abs(gx) < sp * 0.01f;
                bool originY = std::abs(gy) < sp * 0.01f;

                ImU32 colV = originX ? axisYColor : IM_COL32(80, 80, 80, a);
                ImU32 colH = originY ? axisXColor : IM_COL32(80, 80, 80, a);

                DrawLineWorld(dl, { gx, -kHugeExtent, 0 }, { gx, kHugeExtent, 0 }, colV);
                DrawLineWorld(dl, { -kHugeExtent, gy, 0 }, { kHugeExtent, gy, 0 }, colH);
            }
        } else {
            float cx = std::floor(camPos.x / sp) * sp;
            float cz = std::floor(camPos.z / sp) * sp;

            for (int i = -count; i <= count; i++) {
                float gx = cx + i * sp;
                float gz = cz + i * sp;

                float fx = 1.0f - std::abs((float)i) / (float)count;
                fx = fx * fx;
                int a = static_cast<int>(fx * alpha);
                if (a <= 0) continue;

                bool originX = std::abs(gx) < sp * 0.01f;
                bool originZ = std::abs(gz) < sp * 0.01f;

                ImU32 colZ = originX ? axisZColor : IM_COL32(80, 80, 80, a);
                ImU32 colX = originZ ? axisXColor : IM_COL32(80, 80, 80, a);

                DrawLineWorld(dl, { gx, 0, -kHugeExtent }, { gx, 0, kHugeExtent }, colZ);
                DrawLineWorld(dl, { -kHugeExtent, 0, gz }, { kHugeExtent, 0, gz }, colX);
            }
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

    ImDrawList* dl = s_DrawList;

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

void GizmoRenderer::DrawPointLightGizmo(const glm::vec3& position, float range,
                                         const glm::vec3& color) {
    ImDrawList* dl = s_DrawList;

    // Convert light color to ImU32 (clamped, semi-transparent)
    ImU32 wireColor = IM_COL32(
        (int)(std::min(color.x, 1.0f) * 200 + 55),
        (int)(std::min(color.y, 1.0f) * 200 + 55),
        (int)(std::min(color.z, 1.0f) * 200 + 55),
        180);
    ImU32 dotColor = IM_COL32(
        (int)(std::min(color.x, 1.0f) * 255),
        (int)(std::min(color.y, 1.0f) * 255),
        (int)(std::min(color.z, 1.0f) * 255),
        255);

    // Draw center dot
    ImVec2 center = WorldToScreen(position);
    dl->AddCircleFilled(center, 5.0f, dotColor);

    // Draw 3 wireframe circles (one per plane) to show range
    constexpr int SEGMENTS = 32;

    auto drawCircle = [&](auto getPoint) {
        for (int i = 0; i < SEGMENTS; ++i) {
            float a0 = (float)i / SEGMENTS * 2.0f * 3.14159265f;
            float a1 = (float)(i + 1) / SEGMENTS * 2.0f * 3.14159265f;
            DrawLineWorld(dl, getPoint(a0), getPoint(a1), wireColor, 1.0f);
        }
    };

    // XY circle
    drawCircle([&](float a) -> glm::vec3 {
        return position + glm::vec3(std::cos(a), std::sin(a), 0.0f) * range;
    });

    // XZ circle
    drawCircle([&](float a) -> glm::vec3 {
        return position + glm::vec3(std::cos(a), 0.0f, std::sin(a)) * range;
    });

    // YZ circle
    drawCircle([&](float a) -> glm::vec3 {
        return position + glm::vec3(0.0f, std::cos(a), std::sin(a)) * range;
    });

    // Draw small cross-hair lines from center along each axis
    float crossLen = range * 0.15f;
    DrawLineWorld(dl, position - glm::vec3(crossLen, 0, 0), position + glm::vec3(crossLen, 0, 0), wireColor, 1.0f);
    DrawLineWorld(dl, position - glm::vec3(0, crossLen, 0), position + glm::vec3(0, crossLen, 0), wireColor, 1.0f);
    DrawLineWorld(dl, position - glm::vec3(0, 0, crossLen), position + glm::vec3(0, 0, crossLen), wireColor, 1.0f);
}

void GizmoRenderer::DrawCameraFrustum(const glm::mat4& worldTransform,
                                       int projType, float fov, float size,
                                       float nearClip, float farClip, float aspect) {
    ImDrawList* dl = s_DrawList;
    ImU32 wireColor = IM_COL32(220, 220, 220, 160);

    // Compute near/far plane half-extents
    float nearH, nearW, farH, farW;
    if (projType == 0) { // Perspective
        float halfFovRad = glm::radians(fov * 0.5f);
        nearH = nearClip * std::tan(halfFovRad);
        nearW = nearH * aspect;
        farH  = farClip * std::tan(halfFovRad);
        farW  = farH * aspect;
    } else { // Orthographic
        nearH = farH = size;
        nearW = farW = size * aspect;
    }

    // 8 corners in camera local space (camera looks down -Z)
    glm::vec3 local[8] = {
        // Near plane
        { -nearW, -nearH, -nearClip },
        {  nearW, -nearH, -nearClip },
        {  nearW,  nearH, -nearClip },
        { -nearW,  nearH, -nearClip },
        // Far plane (clamped for visualization)
        { -farW, -farH, -std::min(farClip, 50.0f) },
        {  farW, -farH, -std::min(farClip, 50.0f) },
        {  farW,  farH, -std::min(farClip, 50.0f) },
        { -farW,  farH, -std::min(farClip, 50.0f) },
    };

    // Recompute far extents with clamped distance for perspective
    if (projType == 0) {
        float clampedFar = std::min(farClip, 50.0f);
        float halfFovRad = glm::radians(fov * 0.5f);
        float cFarH = clampedFar * std::tan(halfFovRad);
        float cFarW = cFarH * aspect;
        local[4] = { -cFarW, -cFarH, -clampedFar };
        local[5] = {  cFarW, -cFarH, -clampedFar };
        local[6] = {  cFarW,  cFarH, -clampedFar };
        local[7] = { -cFarW,  cFarH, -clampedFar };
    }

    // Transform to world space
    glm::vec3 corners[8];
    for (int i = 0; i < 8; i++)
        corners[i] = glm::vec3(worldTransform * glm::vec4(local[i], 1.0f));

    // Draw 12 edges
    // Near plane
    for (int i = 0; i < 4; i++)
        DrawLineWorld(dl, corners[i], corners[(i + 1) % 4], wireColor, 1.5f);
    // Far plane
    for (int i = 0; i < 4; i++)
        DrawLineWorld(dl, corners[4 + i], corners[4 + (i + 1) % 4], wireColor, 1.5f);
    // Connecting edges
    for (int i = 0; i < 4; i++)
        DrawLineWorld(dl, corners[i], corners[i + 4], wireColor, 1.0f);

    // Draw camera icon (small triangle at position pointing forward)
    glm::vec3 camPos = glm::vec3(worldTransform[3]);
    ImVec2 center = WorldToScreen(camPos);
    dl->AddCircleFilled(center, 4.0f, IM_COL32(255, 255, 255, 200));
}

void GizmoRenderer::DrawWireframeBox(const glm::mat4& worldMatrix) {
    ImDrawList* dl = s_DrawList;
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
