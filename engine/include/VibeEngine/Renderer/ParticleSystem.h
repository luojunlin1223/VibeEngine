/*
 * ParticleSystem — GPU-efficient particle renderer using batched dynamic VBO.
 *
 * Design: Uses the same batched-quad approach as SpriteBatchRenderer but with
 * a dedicated Particle shader that adds soft circular falloff for untextured
 * particles. Particles are sorted back-to-front per emitter for correct alpha
 * blending. Billboard quads always face the camera.
 *
 * Performance target: 10,000+ particles at 60fps via batch rendering (up to
 * 10,000 quads per draw call with automatic flushing).
 */
#pragma once

#include "VibeEngine/Renderer/Texture.h"
#include "VibeEngine/Renderer/VertexArray.h"
#include "VibeEngine/Renderer/Buffer.h"
#include "VibeEngine/Renderer/Shader.h"

#include <glm/glm.hpp>
#include <memory>
#include <vector>

namespace VE {

// Forward declare — full definition in Components.h
struct Particle;
struct ParticleSystemComponent;

class ParticleRenderer {
public:
    static void Init();
    static void Shutdown();

    /// Begin a particle rendering pass. Sets up blending and shader state.
    static void BeginBatch(const glm::mat4& viewProjection);

    /// Flush current batch to GPU and issue draw call.
    static void Flush();

    /// End the rendering pass. Flushes remaining quads.
    static void EndBatch();

    /// Sort particles back-to-front relative to camera, then draw billboard quads.
    /// Uses the same vertex format as SpriteBatchRenderer for compatibility.
    static void DrawParticles(const std::vector<Particle>& particles,
                              const glm::vec3& cameraPos,
                              const std::shared_ptr<Texture2D>& texture);

    struct Stats {
        uint32_t DrawCalls = 0;
        uint32_t QuadCount = 0;
    };
    static Stats GetStats();
    static void ResetStats();

private:
    static void EmitQuad(const glm::vec3& position, float size,
                         const glm::vec4& color, const glm::vec3& cameraPos,
                         float texIndex);
};

} // namespace VE
