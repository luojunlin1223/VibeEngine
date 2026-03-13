/*
 * EditorCamera — Dual-mode editor camera (2D orthographic / 3D perspective).
 *
 * Controls:
 *   2D mode: middle-mouse pan, scroll zoom
 *   3D mode: right-mouse orbit, middle-mouse pan, scroll dolly
 */
#pragma once

#include <glm/glm.hpp>

namespace VE {

enum class CameraMode { Orthographic2D, Perspective3D };

class EditorCamera {
public:
    EditorCamera() { RecalculateMatrix(); }

    void SetViewportSize(float width, float height);
    void SetMode(CameraMode mode);

    void OnMouseScroll(float yOffset);
    void OnMouseDrag(float dx, float dy);   // middle-mouse pan
    void OnMouseRotate(float dx, float dy); // right-mouse orbit (3D only)

    const glm::mat4& GetViewProjection() const { return m_ViewProjection; }
    const glm::mat4& GetSkyViewProjection() const { return m_SkyViewProjection; }
    const glm::mat4& GetViewMatrix() const { return m_ViewMatrix; }
    const glm::mat4& GetProjectionMatrix() const { return m_ProjectionMatrix; }
    CameraMode GetMode() const { return m_Mode; }
    float GetNearClip() const { return m_NearClip; }
    float GetFarClip() const { return m_FarClip; }

    // 2D accessors
    float GetZoom() const { return m_Zoom; }
    const glm::vec2& GetPosition() const { return m_Position2D; }

    // 3D accessors
    const glm::vec3& GetPosition3D() const { return m_Position3D; }
    const glm::vec3& GetFocalPoint() const { return m_FocalPoint; }
    float GetDistance() const { return m_Distance; }
    float GetYaw() const { return m_Yaw; }
    float GetPitch() const { return m_Pitch; }
    float GetFOV() const { return m_FOV; }

    // Setters for restoring saved state
    void SetPosition2D(const glm::vec2& pos) { m_Position2D = pos; RecalculateMatrix(); }
    void SetZoom(float z) { m_Zoom = z; RecalculateMatrix(); }
    void SetFocalPoint(const glm::vec3& fp) { m_FocalPoint = fp; RecalculatePosition3D(); RecalculateMatrix(); }
    void SetDistance(float d) { m_Distance = d; RecalculatePosition3D(); RecalculateMatrix(); }
    void SetYaw(float y) { m_Yaw = y; RecalculatePosition3D(); RecalculateMatrix(); }
    void SetPitch(float p) { m_Pitch = p; RecalculatePosition3D(); RecalculateMatrix(); }

private:
    void RecalculateMatrix();
    void RecalculatePosition3D();
    glm::vec3 GetForwardDir() const;
    glm::vec3 GetRightDir() const;
    glm::vec3 GetUpDir() const;

    CameraMode m_Mode = CameraMode::Perspective3D;
    float m_AspectRatio = 16.0f / 9.0f;
    glm::mat4 m_ViewProjection    = glm::mat4(1.0f);
    glm::mat4 m_SkyViewProjection = glm::mat4(1.0f);
    glm::mat4 m_ViewMatrix        = glm::mat4(1.0f);
    glm::mat4 m_ProjectionMatrix  = glm::mat4(1.0f);

    // 2D state
    glm::vec2 m_Position2D = { 0.0f, 0.0f };
    float m_Zoom = 1.0f;

    // 3D state
    glm::vec3 m_FocalPoint = { 0.0f, 0.0f, 0.0f };
    float m_Distance = 5.0f;
    float m_Yaw   = -45.0f;  // degrees
    float m_Pitch =  30.0f;  // degrees
    float m_FOV   = 45.0f;   // degrees
    float m_NearClip = 0.1f;
    float m_FarClip  = 1000.0f;
    glm::vec3 m_Position3D = { 0.0f, 0.0f, 0.0f };
};

} // namespace VE
