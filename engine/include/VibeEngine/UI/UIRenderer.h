/*
 * UIRenderer — Batched screen-space UI rendering.
 *
 * Similar to SpriteBatchRenderer but uses a screen-space orthographic projection.
 * Renders colored quads, textured quads, and text glyphs in minimal draw calls.
 * All coordinates are in screen pixels (origin top-left).
 */
#pragma once

#include "VibeEngine/UI/FontAtlas.h"
#include "VibeEngine/Renderer/Texture.h"
#include <glm/glm.hpp>
#include <string>
#include <memory>

namespace VE {

enum class UIAnchor {
    TopLeft, TopCenter, TopRight,
    MiddleLeft, Center, MiddleRight,
    BottomLeft, BottomCenter, BottomRight
};

class UIRenderer {
public:
    static void Init();
    static void Shutdown();

    // Begin a UI rendering frame — sets up screen-space ortho projection
    static void BeginFrame(uint32_t screenWidth, uint32_t screenHeight);
    static void EndFrame();

    // Colored rectangle (screen pixels, origin top-left)
    static void DrawRect(float x, float y, float width, float height,
                         const glm::vec4& color);

    // Textured rectangle
    static void DrawImage(float x, float y, float width, float height,
                          const std::shared_ptr<Texture2D>& texture,
                          const glm::vec4& tint = glm::vec4(1.0f));

    // Text rendering
    static void DrawText(const std::string& text, float x, float y,
                         float fontSize,
                         const glm::vec4& color = glm::vec4(1.0f),
                         const std::shared_ptr<FontAtlas>& font = nullptr);

    // Input state (call before BeginFrame)
    static void SetMouseState(float mouseX, float mouseY, bool mouseDown);

    // Hit test — check if point is within rect
    static bool IsMouseOver(float x, float y, float width, float height);
    static bool IsMouseClicked(float x, float y, float width, float height);

    // Flush current batch to GPU
    static void Flush();

    struct Stats {
        uint32_t DrawCalls = 0;
        uint32_t QuadCount = 0;
    };
    static Stats GetStats();
    static void ResetStats();

private:
    static void StartNewBatch();
};

} // namespace VE
