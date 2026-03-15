/*
 * PostProcessing — Fullscreen post-processing effects pipeline.
 *
 * Supported effects (applied in this order in the composite pass):
 *   1. Bloom: brightness extraction → multi-pass Gaussian blur → additive composite
 *   2. Color Curves: per-channel (Master/R/G/B) remapping via 1D LUT
 *   3. Shadows/Midtones/Highlights: tonal-range color grading
 *   4. Color Adjustments: exposure, contrast, saturation, color filter
 *   5. Tonemapping: HDR → LDR (Reinhard / ACES / Uncharted 2)
 *   6. Gamma correction
 *   7. Vignette: darkened screen edges
 *
 * OpenGL-only for now. Uses internal shaders and ping-pong framebuffers.
 */
#pragma once

#include <cstdint>
#include <array>
#include <vector>
#include <utility>
#include <glm/glm.hpp>

namespace VE {

struct BloomSettings {
    bool Enabled = false;
    float Threshold = 0.8f;
    float Intensity = 1.0f;
    int Iterations = 5;
};

struct VignetteSettings {
    bool Enabled = false;
    float Intensity = 0.5f;
    float Smoothness = 0.5f;
};

struct ColorAdjustments {
    bool Enabled = false;
    float Exposure = 0.0f;
    float Contrast = 0.0f;
    float Saturation = 0.0f;
    std::array<float, 3> ColorFilter = { 1.0f, 1.0f, 1.0f };
    float Gamma = 1.0f;
};

struct SMHSettings {
    bool Enabled = false;
    std::array<float, 3> Shadows    = { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> Midtones   = { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> Highlights = { 1.0f, 1.0f, 1.0f };
    float ShadowStart    = 0.0f;
    float ShadowEnd      = 0.3f;
    float HighlightStart = 0.55f;
    float HighlightEnd   = 1.0f;
};

/// A curve channel defined by sorted control points (x,y) in [0,1].
struct CurveChannel {
    std::vector<std::pair<float, float>> Points = { {0.0f, 0.0f}, {1.0f, 1.0f} };
};

struct ColorCurvesSettings {
    bool Enabled = false;
    CurveChannel Master;
    CurveChannel Red;
    CurveChannel Green;
    CurveChannel Blue;
};

enum class TonemapMode { None = 0, Reinhard, ACES, Uncharted2 };

struct TonemappingSettings {
    bool Enabled = false;
    TonemapMode Mode = TonemapMode::ACES;
};

enum class AAMethod { None = 0, FXAA, TAA };

struct MotionBlurSettings {
    bool Enabled = false;
    float Strength   = 0.5f;
    int   NumSamples = 8;
    glm::mat4 InvViewProj  = glm::mat4(1.0f); // current frame
    glm::mat4 PrevViewProj = glm::mat4(1.0f); // previous frame
    uint32_t  DepthTexture = 0;
};

struct FXAASettings {
    bool Enabled = false;
    float EdgeThreshold    = 0.0833f;
    float EdgeThresholdMin = 0.0625f;
    float SubpixelQuality  = 0.75f;
};

struct TAASettings {
    bool Enabled = false;
    float BlendFactor = 0.1f;  // weight of current frame
};

enum class FogMode { Linear = 0, Exponential, ExponentialSquared };

struct FogSettings {
    bool  Enabled    = false;
    FogMode Mode     = FogMode::ExponentialSquared;
    std::array<float, 3> Color = { 0.7f, 0.75f, 0.8f };
    float Density    = 0.02f;   // for Exp/Exp2
    float Start      = 10.0f;   // for Linear
    float End        = 100.0f;  // for Linear
    float HeightFalloff = 0.0f; // 0 = no height fog, >0 = fog thins with altitude
    float MaxOpacity = 1.0f;    // clamp max fog factor
};

struct VolumetricFogSettings {
    bool  Enabled      = false;
    float Density      = 0.015f;
    float Scattering   = 0.7f;   // Henyey-Greenstein asymmetry (0=isotropic, 1=forward)
    float LightIntensity = 1.0f; // god-ray strength
    std::array<float, 3> Color = { 0.9f, 0.9f, 1.0f };
    int   Steps        = 32;     // ray march steps (quality)
    float MaxDistance   = 100.0f;
    float HeightFalloff = 0.05f; // density decreases with height
    float BaseHeight    = 0.0f;  // world Y where fog is densest
};

struct DoFSettings {
    bool  Enabled       = false;
    float FocusDistance  = 10.0f;  // world-space distance from camera
    float FocusRange    = 5.0f;   // smooth transition zone (half-width)
    float MaxBlur       = 4.0f;   // maximum blur radius in pixels
    float ApertureSize  = 0.05f;  // controls overall blur strength
};

struct PostProcessSettings {
    BloomSettings       Bloom;
    VignetteSettings    Vignette;
    ColorAdjustments    Color;
    SMHSettings         SMH;
    ColorCurvesSettings Curves;
    TonemappingSettings Tonemap;
    MotionBlurSettings  MotionBlur;
    FXAASettings        FXAA;
    TAASettings         TAA;
    FogSettings         Fog;
    VolumetricFogSettings VolumetricFog;
    DoFSettings         DoF;
    uint32_t            SSAOTexture = 0;
    uint32_t            SSRTexture  = 0;   // RGBA16F: RGB=reflected color, A=confidence
    uint32_t            DepthTexture = 0;
    float               NearClip = 0.1f;
    float               FarClip  = 1000.0f;
    // For volumetric fog ray marching (view-space reconstruction)
    glm::mat4           InvProjection = glm::mat4(1.0f);
    glm::mat4           InvView       = glm::mat4(1.0f);
    glm::vec3           LightDir      = glm::vec3(0.3f, 1.0f, 0.5f);
};

class PostProcessing {
public:
    PostProcessing() = default;
    ~PostProcessing();

    void Init(uint32_t width, uint32_t height);
    void Shutdown();
    void Resize(uint32_t width, uint32_t height);

    uint32_t Apply(uint32_t sceneColorTexture, uint32_t width, uint32_t height,
                   const PostProcessSettings& settings);

private:
    void CreateResources();
    void DestroyResources();
    void CompileShaders();
    void RenderFullscreenQuad();

    void BakeCurvesLUT(const ColorCurvesSettings& curves);

    uint32_t m_Width = 0, m_Height = 0;
    bool m_Initialized = false;

    uint32_t m_QuadVAO = 0;

    uint32_t m_BrightExtractShader = 0;
    uint32_t m_BlurShader = 0;
    uint32_t m_CompositeShader = 0;
    uint32_t m_FXAAShader = 0;
    uint32_t m_TAAShader = 0;
    uint32_t m_MotionBlurShader = 0;
    uint32_t m_VolFogShader = 0;
    uint32_t m_DoFShader = 0;

    uint32_t m_BrightFBO = 0, m_BrightTexture = 0;
    uint32_t m_BlurFBO[2] = { 0, 0 };
    uint32_t m_BlurTexture[2] = { 0, 0 };
    uint32_t m_CompositeFBO = 0, m_CompositeTexture = 0;

    // Volumetric fog output
    uint32_t m_VolFogFBO = 0, m_VolFogTexture = 0;

    // Depth of Field output (two-pass: H then V)
    uint32_t m_DoFFBO[2] = { 0, 0 };
    uint32_t m_DoFTexture[2] = { 0, 0 };

    // FXAA output
    uint32_t m_FXAAFBO = 0, m_FXAATexture = 0;

    // TAA history (ping-pong)
    uint32_t m_TAAHistoryFBO[2] = { 0, 0 };
    uint32_t m_TAAHistoryTex[2] = { 0, 0 };
    int      m_TAACurrentIdx = 0;
    bool     m_TAAFirstFrame = true;
    uint32_t m_TAAFBO = 0, m_TAATexture = 0;

    // Color curves 1D LUT (256x1 RGBA)
    uint32_t m_CurvesLUT = 0;
};

} // namespace VE
