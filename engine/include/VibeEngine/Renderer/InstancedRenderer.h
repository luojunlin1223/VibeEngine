/*
 * InstancedRenderer — GPU-instanced mesh rendering.
 *
 * Batches entities that share the same mesh+material into single
 * glDrawElementsInstanced calls. Per-instance data (model matrix, color)
 * is uploaded via a dynamic VBO with vertex attribute divisors.
 */
#pragma once

#include "VibeEngine/Renderer/VertexArray.h"
#include "VibeEngine/Renderer/Buffer.h"
#include "VibeEngine/Renderer/Shader.h"
#include "VibeEngine/Renderer/Material.h"
#include <glm/glm.hpp>
#include <memory>
#include <vector>
#include <map>

namespace VE {

struct InstanceData {
    glm::mat4 Model;   // 64 bytes
    glm::vec4 Color;   // 16 bytes
};                     // 80 bytes total

class InstancedRenderer {
public:
    static void Init();
    static void Shutdown();

    // Begin/End a frame of instanced rendering
    static void BeginScene(const glm::mat4& viewProjection);
    static void EndScene();

    // Submit an instance for batched rendering. Entities sharing the same
    // mesh and material will be drawn in a single instanced draw call.
    static void Submit(const std::shared_ptr<VertexArray>& mesh,
                       const std::shared_ptr<Material>& material,
                       const glm::mat4& model,
                       const glm::vec4& color);

    struct Stats {
        uint32_t DrawCalls = 0;
        uint32_t InstanceCount = 0;
    };
    static Stats GetStats();
    static void ResetStats();

    static std::shared_ptr<Shader> GetUnlitInstancedShader();
    static std::shared_ptr<Shader> GetLitInstancedShader();
};

} // namespace VE
