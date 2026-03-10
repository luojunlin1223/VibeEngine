/*
 * EditorCamera — Simple 2D orthographic camera for the scene editor.
 *
 * Controls:
 *   - Middle mouse drag: pan
 *   - Scroll wheel: zoom
 */
#pragma once

#include <glm/glm.hpp>

namespace VE {

class EditorCamera {
public:
    EditorCamera() = default;

    void SetViewportSize(float width, float height);
    void OnMouseScroll(float yOffset);
    void OnMouseDrag(float dx, float dy);

    const glm::mat4& GetViewProjection() const { return m_ViewProjection; }
    float GetZoom() const { return m_Zoom; }
    const glm::vec2& GetPosition() const { return m_Position; }

private:
    void RecalculateMatrix();

    glm::vec2 m_Position = { 0.0f, 0.0f };
    float m_Zoom = 1.0f;
    float m_AspectRatio = 16.0f / 9.0f;
    glm::mat4 m_ViewProjection = glm::mat4(1.0f);
};

} // namespace VE
