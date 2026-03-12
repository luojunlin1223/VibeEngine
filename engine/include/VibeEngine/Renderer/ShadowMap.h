/*
 * ShadowMap — Cascaded Shadow Map (CSM) with PCF soft shadows.
 *
 * Manages a depth texture array (one layer per cascade), computes
 * light-space matrices from the camera frustum, and provides a
 * depth-only shader for the shadow pass.
 *
 * Usage:
 *   1. Call ComputeCascades() each frame with camera + light info.
 *   2. For each cascade: BeginPass(i) → render scene depth → EndPass().
 *   3. In the main pass: BindForReading(textureUnit) and set uniforms.
 */
#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <memory>
#include <vector>

namespace VE {

class Shader;

class ShadowMap {
public:
    static constexpr int NUM_CASCADES = 3;
    static constexpr int MAP_SIZE = 2048;

    ShadowMap();
    ~ShadowMap();

    // Compute light-space matrices for each cascade from camera frustum
    void ComputeCascades(const glm::mat4& viewMatrix,
                         const glm::mat4& projMatrix,
                         const glm::vec3& lightDir,
                         float nearClip, float farClip);

    // Begin/end shadow depth pass for a specific cascade
    void BeginPass(int cascadeIndex);
    void EndPass();

    // Bind shadow map texture array for sampling in the main render pass
    void BindForReading(uint32_t textureUnit) const;

    // Getters
    const glm::mat4& GetLightSpaceMatrix(int cascade) const { return m_LightSpaceMatrices[cascade]; }
    float GetCascadeSplit(int cascade) const { return m_CascadeSplits[cascade]; }
    const std::shared_ptr<Shader>& GetDepthShader() const { return m_DepthShader; }

private:
    void Init();
    std::vector<glm::vec4> GetFrustumCornersWorldSpace(const glm::mat4& viewProj) const;

    uint32_t m_FBO = 0;
    uint32_t m_DepthTexture = 0; // GL_TEXTURE_2D_ARRAY

    glm::mat4 m_LightSpaceMatrices[NUM_CASCADES];
    float m_CascadeSplits[NUM_CASCADES] = {};

    std::shared_ptr<Shader> m_DepthShader;

    // Saved viewport to restore after shadow pass
    int m_SavedViewport[4] = {};
};

} // namespace VE
