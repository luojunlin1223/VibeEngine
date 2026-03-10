#include "VibeEngine/Renderer/EditorCamera.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

namespace VE {

void EditorCamera::SetViewportSize(float width, float height) {
    if (height > 0.0f)
        m_AspectRatio = width / height;
    RecalculateMatrix();
}

void EditorCamera::OnMouseScroll(float yOffset) {
    m_Zoom -= yOffset * 0.1f * m_Zoom;
    m_Zoom = std::clamp(m_Zoom, 0.01f, 100.0f);
    RecalculateMatrix();
}

void EditorCamera::OnMouseDrag(float dx, float dy) {
    m_Position.x -= dx * m_Zoom * 0.002f;
    m_Position.y += dy * m_Zoom * 0.002f;
    RecalculateMatrix();
}

void EditorCamera::RecalculateMatrix() {
    float left   = -m_AspectRatio * m_Zoom + m_Position.x;
    float right  =  m_AspectRatio * m_Zoom + m_Position.x;
    float bottom = -m_Zoom + m_Position.y;
    float top    =  m_Zoom + m_Position.y;

    m_ViewProjection = glm::ortho(left, right, bottom, top, -10.0f, 10.0f);
}

} // namespace VE
