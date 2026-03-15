/*
 * SSAO — Screen-Space Ambient Occlusion.
 *
 * Samples hemisphere kernel around each fragment in view space,
 * compares depths to detect occlusion, then applies a blur pass.
 * The result is a single-channel AO texture that multiplies the scene color.
 */
#pragma once

#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

namespace VE {

struct SSAOSettings {
    bool  Enabled    = false;
    float Radius     = 0.5f;   // sampling radius in world units
    float Bias       = 0.025f; // depth bias to prevent self-occlusion
    float Intensity  = 1.0f;   // AO strength multiplier
    int   KernelSize = 32;     // number of samples (8-64)
};

class SSAO {
public:
    SSAO() = default;
    ~SSAO();

    void Init(uint32_t width, uint32_t height);
    void Shutdown();
    void Resize(uint32_t width, uint32_t height);

    // Generate AO texture from depth buffer. Returns AO texture ID.
    uint32_t Compute(uint32_t depthTexture, uint32_t width, uint32_t height,
                     const glm::mat4& projection, const glm::mat4& view,
                     const SSAOSettings& settings);

    uint32_t GetAOTexture() const { return m_BlurTexture; }

private:
    void CreateResources();
    void DestroyResources();
    void CompileShaders();
    void GenerateKernel();
    void GenerateNoiseTexture();
    void RenderFullscreenQuad();

    uint32_t m_Width = 0, m_Height = 0;
    bool m_Initialized = false;

    uint32_t m_QuadVAO = 0;

    // Shaders
    uint32_t m_SSAOShader = 0;
    uint32_t m_BlurShader = 0;

    // FBOs
    uint32_t m_SSAOFBO = 0, m_SSAOTexture = 0;
    uint32_t m_BlurFBO = 0, m_BlurTexture = 0;

    // Kernel + noise
    std::vector<glm::vec3> m_Kernel;
    uint32_t m_NoiseTexture = 0;

    // Cached uniform locations (resolved once after shader compile)
    bool m_UniformsCached = false;
    int32_t m_LocDepthMap = -1, m_LocNoiseTex = -1;
    int32_t m_LocSamples[64] = {};
    int32_t m_LocKernelSize = -1, m_LocRadius = -1, m_LocBias = -1, m_LocIntensity = -1;
    int32_t m_LocProjection = -1, m_LocView = -1, m_LocScreenSize = -1;
    int32_t m_LocBlurInput = -1;
    void CacheUniformLocations();
};

} // namespace VE
