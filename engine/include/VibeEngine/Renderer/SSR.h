/*
 * SSR — Screen-Space Reflections.
 *
 * Ray-marches along each fragment's reflection direction in screen space,
 * sampling the depth buffer to detect intersections.  Uses hierarchical
 * stepping (large strides first, then binary-search refinement) for
 * performance.  The result is an RGBA texture whose RGB channels hold the
 * reflected color and whose alpha encodes reflection confidence (used for
 * blending with the original scene).
 */
#pragma once

#include <cstdint>
#include <glm/glm.hpp>

namespace VE {

struct SSRSettings {
    bool  Enabled   = false;
    int   MaxSteps  = 64;      // maximum ray-march steps
    float StepSize  = 0.05f;   // initial step size in UV space
    float Thickness = 0.1f;    // depth-comparison thickness (view-space units)
    float MaxDistance = 50.0f;  // max ray travel distance in view space
};

class SSR {
public:
    SSR() = default;
    ~SSR();

    void Init(uint32_t width, uint32_t height);
    void Shutdown();
    void Resize(uint32_t width, uint32_t height);

    /// Compute the SSR texture from scene color + depth.
    /// Returns the reflection texture ID (RGBA16F).
    uint32_t Compute(uint32_t colorTexture, uint32_t depthTexture,
                     uint32_t width, uint32_t height,
                     const glm::mat4& projection, const glm::mat4& view,
                     const SSRSettings& settings);

    uint32_t GetReflectionTexture() const { return m_ReflectionTexture; }

private:
    void CreateResources();
    void DestroyResources();
    void CompileShaders();
    void CacheUniformLocations();
    void RenderFullscreenQuad();

    uint32_t m_Width = 0, m_Height = 0;
    bool m_Initialized = false;

    uint32_t m_QuadVAO = 0;

    // Shaders
    uint32_t m_SSRShader = 0;

    // Cached uniform locations (looked up once after shader compilation)
    int32_t m_LocSceneColor    = -1;
    int32_t m_LocDepthMap      = -1;
    int32_t m_LocProjection    = -1;
    int32_t m_LocInvProjection = -1;
    int32_t m_LocView          = -1;
    int32_t m_LocScreenSize    = -1;
    int32_t m_LocMaxSteps      = -1;
    int32_t m_LocStepSize      = -1;
    int32_t m_LocThickness     = -1;
    int32_t m_LocMaxDistance    = -1;

    // FBOs
    uint32_t m_SSRFBO = 0, m_ReflectionTexture = 0;
};

} // namespace VE
