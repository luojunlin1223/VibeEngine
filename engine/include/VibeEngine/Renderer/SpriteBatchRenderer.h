/*
 * SpriteBatchRenderer — Efficient batched 2D sprite rendering.
 *
 * Collects sprite quads into a dynamic VBO and draws them in minimal
 * draw calls. Supports up to 16 texture slots per batch with automatic
 * flushing when capacity is reached.
 */
#pragma once

#include "VibeEngine/Renderer/Texture.h"
#include <glm/glm.hpp>
#include <array>
#include <memory>

namespace VE {

class SpriteBatchRenderer {
public:
    static void Init();
    static void Shutdown();

    static void BeginBatch(const glm::mat4& viewProjection);
    static void EndBatch();
    static void Flush();

    static void DrawSprite(const glm::mat4& transform,
                           const glm::vec4& color,
                           const std::shared_ptr<Texture2D>& texture,
                           const std::array<float, 4>& uvRect);

    struct Stats {
        uint32_t DrawCalls = 0;
        uint32_t QuadCount = 0;
    };
    static Stats GetStats();
    static void ResetStats();
};

} // namespace VE
