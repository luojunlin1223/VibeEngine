/*
 * GizmoRenderer — Draws Unity-style gizmos.
 *
 * Depth-tested lines (grid, wireframes, light gizmos) are drawn via
 * OpenGL GL_LINES with depth read-only, so they are properly occluded
 * by scene geometry.  Translation gizmo arrows remain on the ImGui
 * draw list so the user can always see and grab them.
 *
 * Supports both 2D (XY plane) and 3D (XZ plane) grid modes.
 */
#pragma once

#include "VibeEngine/Scene/Entity.h"
#include "VibeEngine/Renderer/EditorCamera.h"
#include <glm/glm.hpp>
#include <vector>
#include <cstdint>

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

    // Collider wireframes (semi-transparent green, Unity-style)
    // color: RGBA in 0..1; default (0,0,0,0) uses green (0, 1, 0, 0.6)
    static void DrawWireframeBoxCollider(const glm::vec3& center, const glm::vec3& size,
                                         const glm::mat3& rotation = glm::mat3(1.0f),
                                         const glm::vec4& color = glm::vec4(0.0f));
    static void DrawWireframeSphereCollider(const glm::vec3& center, float radius,
                                            const glm::vec4& color = glm::vec4(0.0f));
    static void DrawWireframeCapsuleCollider(const glm::vec3& center, float radius, float height,
                                             const glm::mat3& rotation = glm::mat3(1.0f),
                                             const glm::vec4& color = glm::vec4(0.0f));

    // Screen ↔ World conversion
    static glm::vec2 ScreenToWorld(float screenX, float screenY);

    // Project a world-space point to screen-space (ImGui coordinates).
    // Returns {-1,-1} if the point is behind the camera.
    static glm::vec2 ProjectWorldToScreen(const glm::vec3& worldPos);

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

    // ── Depth-tested 3D line rendering ────────────────────────────
    struct GizmoLine {
        glm::vec3 Start, End;
        glm::vec4 Color;
    };

    /// Queue a world-space line for depth-tested rendering.
    static void AddLine3D(const glm::vec3& start, const glm::vec3& end, const glm::vec4& color);

    /// Upload queued lines and draw them with GL_LINES.
    /// Call this while the scene framebuffer is bound with depth test enabled.
    static void FlushLines3D(const glm::mat4& viewProjection);
    static bool IsLines3DEmpty();

private:
    static std::vector<GizmoLine> s_Lines3D;
    static uint32_t s_LineShaderProgram;
    static uint32_t s_LineVAO, s_LineVBO;
    static bool     s_LineRendererInited;
    static void InitLineRenderer();
};

} // namespace VE
