#include "VibeEngine/Renderer/EditorCamera.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace VE {

void EditorCamera::SetViewportSize(float width, float height) {
    if (height > 0.0f)
        m_AspectRatio = width / height;
    RecalculateMatrix();
}

void EditorCamera::SetMode(CameraMode mode) {
    m_Mode = mode;
    RecalculateMatrix();
}

void EditorCamera::OnMouseScroll(float yOffset) {
    if (m_Mode == CameraMode::Orthographic2D) {
        m_Zoom -= yOffset * 0.1f * m_Zoom;
        m_Zoom = std::clamp(m_Zoom, 0.01f, 100.0f);
    } else {
        m_Distance -= yOffset * 0.15f * m_Distance;
        m_Distance = std::clamp(m_Distance, 0.1f, 500.0f);
    }
    RecalculateMatrix();
}

void EditorCamera::OnMouseDrag(float dx, float dy) {
    if (m_Mode == CameraMode::Orthographic2D) {
        m_Position2D.x -= dx * m_Zoom * 0.002f;
        m_Position2D.y += dy * m_Zoom * 0.002f;
    } else {
        // Pan focal point along camera right/up vectors
        float panSpeed = m_Distance * 0.002f;
        m_FocalPoint -= GetRightDir() * dx * panSpeed;
        m_FocalPoint += GetUpDir()    * dy * panSpeed;
    }
    RecalculateMatrix();
}

void EditorCamera::OnMouseRotate(float dx, float dy) {
    if (m_Mode != CameraMode::Perspective3D) return;

    m_Yaw   -= dx * 0.3f;
    m_Pitch -= dy * 0.3f;
    m_Pitch = std::clamp(m_Pitch, -89.0f, 89.0f);
    RecalculateMatrix();
}

void EditorCamera::RecalculatePosition3D() {
    float yawRad   = glm::radians(m_Yaw);
    float pitchRad = glm::radians(m_Pitch);

    m_Position3D.x = m_FocalPoint.x + m_Distance * std::cos(pitchRad) * std::cos(yawRad);
    m_Position3D.y = m_FocalPoint.y + m_Distance * std::sin(pitchRad);
    m_Position3D.z = m_FocalPoint.z + m_Distance * std::cos(pitchRad) * std::sin(yawRad);
}

glm::vec3 EditorCamera::GetForwardDir() const {
    return glm::normalize(m_FocalPoint - m_Position3D);
}

glm::vec3 EditorCamera::GetRightDir() const {
    return glm::normalize(glm::cross(GetForwardDir(), glm::vec3(0, 1, 0)));
}

glm::vec3 EditorCamera::GetUpDir() const {
    return glm::normalize(glm::cross(GetRightDir(), GetForwardDir()));
}

void EditorCamera::RecalculateMatrix() {
    if (m_Mode == CameraMode::Orthographic2D) {
        float left   = -m_AspectRatio * m_Zoom + m_Position2D.x;
        float right  =  m_AspectRatio * m_Zoom + m_Position2D.x;
        float bottom = -m_Zoom + m_Position2D.y;
        float top    =  m_Zoom + m_Position2D.y;
        m_ViewProjection = glm::ortho(left, right, bottom, top, -10.0f, 10.0f);
    } else {
        RecalculatePosition3D();
        glm::mat4 proj = glm::perspective(glm::radians(m_FOV), m_AspectRatio, m_NearClip, m_FarClip);
        glm::mat4 view = glm::lookAt(m_Position3D, m_FocalPoint, glm::vec3(0, 1, 0));
        m_ViewProjection = proj * view;
        // Sky VP strips translation so the sky sphere stays centered on camera
        m_SkyViewProjection = proj * glm::mat4(glm::mat3(view));
    }
}

} // namespace VE
