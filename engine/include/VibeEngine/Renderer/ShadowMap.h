/*
 * ShadowMap — Cascaded Shadow Maps (CSM) for deferred rendering.
 *
 * Manages a single GL_TEXTURE_2D_ARRAY with NUM_CASCADES depth layers.
 * Each cascade covers a different portion of the camera frustum, providing
 * high-resolution shadows near the camera and lower resolution far away.
 */
#pragma once

#include <glm/glm.hpp>
#include <array>
#include <memory>
#include <cstdint>

namespace VE {

class Shader;

struct ShadowSettings {
    bool  Enabled            = true;
    int   Resolution         = 2048;
    float MaxDistance         = 200.0f;
    float SplitLambda        = 0.75f;   // 0=uniform, 1=logarithmic
    float DepthBias          = 0.005f;
    float NormalBias         = 0.02f;
    int   PCFQuality         = 1;       // 0=Hard, 1=3x3, 2=5x5
    float CascadeBlendWidth  = 0.1f;
};

class ShadowMap {
public:
    static constexpr int NUM_CASCADES  = 4;
    static constexpr int TEXTURE_UNIT  = 8;

    ShadowMap() = default;
    ~ShadowMap();

    void Init(int resolution = 2048);
    void Shutdown();
    void Resize(int resolution);

    bool IsInitialized() const { return m_Initialized; }
    int  GetResolution()  const { return m_Resolution; }

    /// Compute cascade splits and light-space matrices for this frame.
    void Update(const glm::mat4& view, const glm::mat4& projection,
                const glm::vec3& lightDir, float nearClip, float farClip,
                const ShadowSettings& settings);

    /// Begin/end shadow depth rendering for one cascade layer.
    void BeginCascadePass(int cascadeIndex);
    void EndCascadePass();

    /// Bind shadow map array to TEXTURE_UNIT and set all shadow uniforms on shader.
    void BindToShader(const std::shared_ptr<Shader>& shader, const ShadowSettings& settings);

    /// Get the internal depth-only shader for rendering shadow casters.
    std::shared_ptr<Shader> GetDepthShader();

    const glm::mat4& GetLightViewProjection(int cascadeIndex) const {
        return m_LightVP[cascadeIndex];
    }

    const std::array<float, NUM_CASCADES>& GetCascadeSplitDistances() const {
        return m_CascadeSplits;
    }

    uint32_t GetTextureArrayID() const { return m_DepthTexArray; }

private:
    void CreateGLResources();
    void DestroyGLResources();

    void ComputeCascadeSplits(float nearClip, float farClip, float lambda);
    glm::mat4 ComputeLightMatrix(const glm::mat4& view, const glm::mat4& projection,
                                  const glm::vec3& lightDir,
                                  float splitNear, float splitFar);

    bool     m_Initialized   = false;
    int      m_Resolution    = 2048;

    uint32_t m_FBO           = 0;
    uint32_t m_DepthTexArray = 0;

    std::array<glm::mat4, NUM_CASCADES> m_LightVP{};
    std::array<float, NUM_CASCADES>     m_CascadeSplits{};

    std::shared_ptr<Shader> m_DepthShader;
};

} // namespace VE
