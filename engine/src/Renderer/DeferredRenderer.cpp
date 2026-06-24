/*
 * DeferredRenderer — Implementation of the deferred rendering pipeline.
 *
 * Creates a G-buffer with 4 color attachments (MRT), performs a geometry pass
 * to fill the G-buffer, then a fullscreen lighting pass to produce the final
 * HDR lit color. The output feeds into the existing post-processing chain.
 */
#include "VibeEngine/Renderer/DeferredRenderer.h"
#include "VibeEngine/Renderer/ShaderSources.h"
#include "VibeEngine/Renderer/GPUResourceTracker.h"
#include "VibeEngine/Core/Log.h"
#include <glad/gl.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace VE {

static constexpr GLuint kHPWaterStencilTraceReflectionRay = 1u << 3u;
static constexpr GLuint kHPWaterStencilWaterSurface = 1u << 5u;
static constexpr GLuint kHPWaterStencilRef =
    kHPWaterStencilTraceReflectionRay | kHPWaterStencilWaterSurface;

static uint32_t CalculateMipCount(uint32_t width, uint32_t height) {
    uint32_t maxDim = std::max(width, height);
    uint32_t mipCount = 1;
    while (maxDim > 1) {
        maxDim /= 2;
        ++mipCount;
    }
    return mipCount;
}

static float RadicalInverseVdC(uint32_t bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<float>(bits) * 2.3283064365386963e-10f;
}

static glm::vec2 Hammersley(uint32_t i, uint32_t sampleCount) {
    return glm::vec2(static_cast<float>(i) / static_cast<float>(sampleCount), RadicalInverseVdC(i));
}

static glm::vec3 ImportanceSampleGGX(const glm::vec2& xi, float roughness) {
    constexpr float pi = 3.14159265358979323846f;
    const float a = roughness * roughness;
    const float phi = 2.0f * pi * xi.x;
    const float cosTheta = std::sqrt((1.0f - xi.y) / (1.0f + (a * a - 1.0f) * xi.y));
    const float sinTheta = std::sqrt(std::max(1.0f - cosTheta * cosTheta, 0.0f));
    return glm::vec3(std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta);
}

static float GeometrySchlickGGXIBL(float nDotV, float roughness) {
    const float a = roughness;
    const float k = (a * a) * 0.5f;
    return nDotV / std::max(nDotV * (1.0f - k) + k, 0.0001f);
}

static float GeometrySmithIBL(float nDotV, float nDotL, float roughness) {
    return GeometrySchlickGGXIBL(nDotV, roughness) * GeometrySchlickGGXIBL(nDotL, roughness);
}

static glm::vec2 IntegrateBRDF(float nDotV, float roughness) {
    constexpr uint32_t sampleCount = 128;
    const glm::vec3 v(std::sqrt(std::max(1.0f - nDotV * nDotV, 0.0f)), 0.0f, nDotV);
    float a = 0.0f;
    float b = 0.0f;

    for (uint32_t i = 0; i < sampleCount; ++i) {
        const glm::vec2 xi = Hammersley(i, sampleCount);
        const glm::vec3 h = ImportanceSampleGGX(xi, roughness);
        const glm::vec3 l = glm::normalize(2.0f * glm::dot(v, h) * h - v);

        const float nDotL = std::max(l.z, 0.0f);
        const float nDotH = std::max(h.z, 0.0f);
        const float vDotH = std::max(glm::dot(v, h), 0.0f);

        if (nDotL > 0.0f) {
            const float g = GeometrySmithIBL(nDotV, nDotL, roughness);
            const float gVis = (g * vDotH) / std::max(nDotH * nDotV, 0.0001f);
            const float fc = std::pow(1.0f - vDotH, 5.0f);
            a += (1.0f - fc) * gVis;
            b += fc * gVis;
        }
    }

    const float invSampleCount = 1.0f / static_cast<float>(sampleCount);
    return glm::vec2(a * invSampleCount, b * invSampleCount);
}

// ── Debug visualization fragment shader ─────────────────────────────
// Displays individual G-buffer channels for debugging.

static const char* s_DebugFragSrc = R"(
#version 450 core
layout(location = 0) in vec2 v_UV;
layout(location = 0) out vec4 FragColor;

uniform sampler2D u_GPositionMetallic;
uniform sampler2D u_GNormalRoughness;
uniform sampler2D u_GAlbedoAO;
uniform sampler2D u_GEmissionFlags;
uniform sampler2D u_DepthTexture;
uniform sampler2D u_HPWaterNormalRoughness;
uniform sampler2D u_HPWaterScatterThickness;
uniform sampler2D u_HPWaterAbsorptionFoam;
uniform int u_DebugMode; // 1..8 generic G-buffer, 9..11 HPWater G-buffer

void main() {
    vec4 posMetallic   = texture(u_GPositionMetallic, v_UV);
    vec4 normRoughness = texture(u_GNormalRoughness,  v_UV);
    vec4 albedoAO      = texture(u_GAlbedoAO,         v_UV);
    vec4 emissionFlags = texture(u_GEmissionFlags,    v_UV);

    vec3 result = vec3(0.0);

    if (u_DebugMode == 1) {
        // World position (remap for visibility)
        result = fract(posMetallic.xyz * 0.1);
    } else if (u_DebugMode == 2) {
        // Normals (already in [0,1] from G-buffer encoding)
        result = normRoughness.xyz;
    } else if (u_DebugMode == 3) {
        // Albedo
        result = albedoAO.rgb;
    } else if (u_DebugMode == 4) {
        // Metallic
        result = vec3(posMetallic.w);
    } else if (u_DebugMode == 5) {
        // Roughness
        result = vec3(normRoughness.w);
    } else if (u_DebugMode == 6) {
        // Ambient Occlusion
        result = vec3(albedoAO.a);
    } else if (u_DebugMode == 7) {
        // Emission
        result = emissionFlags.rgb;
    } else if (u_DebugMode == 8) {
        // Depth (linearize for visibility)
        float depth = texture(u_DepthTexture, v_UV).r;
        // Simple non-linear depth visualization
        float near = 0.1;
        float far = 1000.0;
        float linearDepth = (2.0 * near) / (far + near - depth * (far - near));
        result = vec3(linearDepth);
    } else if (u_DebugMode == 9) {
        result = texture(u_HPWaterNormalRoughness, v_UV).xyz;
    } else if (u_DebugMode == 10) {
        result = texture(u_HPWaterScatterThickness, v_UV).rgb;
    } else if (u_DebugMode == 11) {
        result = texture(u_HPWaterAbsorptionFoam, v_UV).rgb;
    }

    FragColor = vec4(result, 1.0);
}
)";

void DeferredRenderer::Init(uint32_t width, uint32_t height) {
    if (m_Initialized) Shutdown();

    m_Width = width;
    m_Height = height;

    // ── Create G-buffer FBO with 4 color attachments (MRT) ──
    FramebufferSpec gbufferSpec;
    gbufferSpec.Width = width;
    gbufferSpec.Height = height;
    gbufferSpec.ColorFormats = {
        { GL_RGBA16F }, // RT0: Position + Metallic
        { GL_RGBA16F }, // RT1: Normal + Roughness
        { GL_RGBA8   }, // RT2: Albedo + AO
        { GL_RGBA8   }, // RT3: Emission + Flags
    };
    m_GBuffer = Framebuffer::Create(gbufferSpec);
    CreateHPWaterGBuffer();
    CreateHPWaterMaskFBO();

    // ── Create lighting output FBO ──
    CreateLightingFBO();
    CreateHPWaterCompositeFBO();
    CreateHPWaterVolumeFBO();
    CreateHPWaterCausticFBO();
    CreateHPWaterCausticComputeTexture();
    CreateHPWaterFGDLUT();
    CreateHPWaterDepthPyramid();

    // ── Load G-Buffer shader ──
    m_GBufferShader = Shader::CreateFromFile("shaders/GBuffer.shader");
    if (!m_GBufferShader) {
        VE_ENGINE_ERROR("DeferredRenderer: Failed to load GBuffer.shader");
    }

    // ── Load HPWater G-buffer shader ──
    m_HPWaterGBufferShader = Shader::CreateFromFile("shaders/HPWaterGBuffer.shader");
    if (!m_HPWaterGBufferShader) {
        VE_ENGINE_ERROR("DeferredRenderer: Failed to load HPWaterGBuffer.shader");
    }

    m_HPWaterMaskShader = Shader::CreateFromFile("shaders/HPWaterMask.shader");
    if (!m_HPWaterMaskShader) {
        VE_ENGINE_ERROR("DeferredRenderer: Failed to load HPWaterMask.shader");
    }

    // ── Load Deferred Lighting shader ──
    m_HPWaterCompositeShader = Shader::CreateFromFile("shaders/HPWaterComposite.shader");
    if (!m_HPWaterCompositeShader) {
        VE_ENGINE_ERROR("DeferredRenderer: Failed to load HPWaterComposite.shader");
    }

    m_HPWaterVolumeShader = Shader::CreateFromFile("shaders/HPWaterVolume.shader");
    if (!m_HPWaterVolumeShader) {
        VE_ENGINE_ERROR("DeferredRenderer: Failed to load HPWaterVolume.shader");
    }

    m_HPWaterVolumeMotionVectorShader = Shader::CreateFromFile("shaders/HPWaterVolumeMotionVector.shader");
    if (!m_HPWaterVolumeMotionVectorShader) {
        VE_ENGINE_ERROR("DeferredRenderer: Failed to load HPWaterVolumeMotionVector.shader");
    }

    m_HPWaterVolumeTemporalShader = Shader::CreateFromFile("shaders/HPWaterVolumeTemporal.shader");
    if (!m_HPWaterVolumeTemporalShader) {
        VE_ENGINE_ERROR("DeferredRenderer: Failed to load HPWaterVolumeTemporal.shader");
    }

    m_HPWaterVolumeFilterShader = Shader::CreateFromFile("shaders/HPWaterVolumeFilter.shader");
    if (!m_HPWaterVolumeFilterShader) {
        VE_ENGINE_ERROR("DeferredRenderer: Failed to load HPWaterVolumeFilter.shader");
    }

    m_HPWaterVolumeUpsampleShader = Shader::CreateFromFile("shaders/HPWaterVolumeUpsample.shader");
    if (!m_HPWaterVolumeUpsampleShader) {
        VE_ENGINE_ERROR("DeferredRenderer: Failed to load HPWaterVolumeUpsample.shader");
    }

    m_HPWaterCausticShader = Shader::CreateFromFile("shaders/HPWaterCaustic.shader");
    if (!m_HPWaterCausticShader) {
        VE_ENGINE_ERROR("DeferredRenderer: Failed to load HPWaterCaustic.shader");
    }

    m_HPWaterCausticFilterShader = Shader::CreateFromFile("shaders/HPWaterCausticFilter.shader");
    if (!m_HPWaterCausticFilterShader) {
        VE_ENGINE_ERROR("DeferredRenderer: Failed to load HPWaterCausticFilter.shader");
    }

    m_HPWaterCausticAtlasShader = Shader::CreateFromFile("shaders/HPWaterCausticAtlas.shader");
    if (!m_HPWaterCausticAtlasShader) {
        VE_ENGINE_ERROR("DeferredRenderer: Failed to load HPWaterCausticAtlas.shader");
    }

    m_HPWaterCausticComputeShader = ComputeShader::CreateFromFile("shaders/HPWaterCausticIrradiance.comp");
    if (!m_HPWaterCausticComputeShader) {
        VE_ENGINE_ERROR("DeferredRenderer: Failed to load HPWaterCausticIrradiance.comp");
    }

    m_HPWaterCausticResolveShader = ComputeShader::CreateFromFile("shaders/HPWaterCausticIrradianceResolve.comp");
    if (!m_HPWaterCausticResolveShader) {
        VE_ENGINE_ERROR("DeferredRenderer: Failed to load HPWaterCausticIrradianceResolve.comp");
    }

    m_HPWaterCausticFilterComputeShader = ComputeShader::CreateFromFile("shaders/HPWaterCausticFilter.comp");
    if (!m_HPWaterCausticFilterComputeShader) {
        VE_ENGINE_ERROR("DeferredRenderer: Failed to load HPWaterCausticFilter.comp");
    }

    m_HPWaterFluidComputeShader = ComputeShader::CreateFromFile("shaders/HPWaterFluidDynamics.comp");
    if (!m_HPWaterFluidComputeShader) {
        VE_ENGINE_ERROR("DeferredRenderer: Failed to load HPWaterFluidDynamics.comp");
    }

    m_HPWaterSpectrumComputeShader = ComputeShader::CreateFromFile("shaders/HPWaterSpectrum.comp");
    if (!m_HPWaterSpectrumComputeShader) {
        VE_ENGINE_ERROR("DeferredRenderer: Failed to load HPWaterSpectrum.comp");
    }

    m_HPWaterDepthPyramidShader = Shader::CreateFromFile("shaders/HPWaterDepthPyramid.shader");
    if (!m_HPWaterDepthPyramidShader) {
        VE_ENGINE_ERROR("DeferredRenderer: Failed to load HPWaterDepthPyramid.shader");
    }

    m_HPWaterDepthMergeShader = Shader::CreateFromFile("shaders/HPWaterDepthMerge.shader");
    if (!m_HPWaterDepthMergeShader) {
        VE_ENGINE_ERROR("DeferredRenderer: Failed to load HPWaterDepthMerge.shader");
    }

    m_HPWaterNormalMergeShader = Shader::CreateFromFile("shaders/HPWaterNormalMerge.shader");
    if (!m_HPWaterNormalMergeShader) {
        VE_ENGINE_ERROR("DeferredRenderer: Failed to load HPWaterNormalMerge.shader");
    }

    m_HPWaterFluidShader = Shader::CreateFromFile("shaders/HPWaterFluidDynamics.shader");
    if (!m_HPWaterFluidShader) {
        VE_ENGINE_ERROR("DeferredRenderer: Failed to load HPWaterFluidDynamics.shader");
    }

    m_HPWaterFluidHeightCaptureShader = Shader::CreateFromFile("shaders/HPWaterFluidHeightCapture.shader");
    if (!m_HPWaterFluidHeightCaptureShader) {
        VE_ENGINE_ERROR("DeferredRenderer: Failed to load HPWaterFluidHeightCapture.shader");
    }

    m_LightingShader = Shader::CreateFromFile("shaders/DeferredLighting.shader");
    if (!m_LightingShader) {
        VE_ENGINE_ERROR("DeferredRenderer: Failed to load DeferredLighting.shader");
    }

    // ── Create debug visualization shader ──
    m_DebugShader = Shader::Create(QuadVertexShaderSrc, s_DebugFragSrc);
    if (m_DebugShader)
        m_DebugShader->SetName("DeferredDebug");

    // ── Dummy VAO for fullscreen triangle ──
    glGenVertexArrays(1, &m_QuadVAO);
    VE_GPU_TRACK(GPUResourceType::VertexArray, m_QuadVAO);

    m_Initialized = true;
    VE_ENGINE_INFO("DeferredRenderer initialized ({}x{}, 4 MRT attachments)", width, height);
}

void DeferredRenderer::Shutdown() {
    DestroyHPWaterDepthPyramid();
    DestroyHPWaterFluidObstacleTexture();
    DestroyHPWaterFluidHeightFieldTextures();
    DestroyHPWaterCausticComputeTexture();
    DestroyHPWaterFGDLUT();

    m_GBuffer.reset();
    m_LightingFBO.reset();
    m_HPWaterGBuffer.reset();
    m_HPWaterMaskFBO.reset();
    m_HPWaterCompositeFBO.reset();
    m_HPWaterVolumeFBO.reset();
    m_HPWaterVolumeMotionVectorFBO.reset();
    m_HPWaterVolumeTemporalFBO.reset();
    m_HPWaterVolumeHistoryFBO.reset();
    m_HPWaterVolumeFilteredFBO.reset();
    m_HPWaterVolumeFilterScratchFBO.reset();
    m_HPWaterVolumeUpsampledFBO.reset();
    m_HPWaterCausticFBO.reset();
    m_HPWaterCausticFilteredFBO.reset();
    m_HPWaterCausticFilterScratchFBO.reset();
    m_HPWaterCausticAtlasFBO.reset();
    m_HPWaterFluidCurrentFBO.reset();
    m_HPWaterFluidPreviousFBO.reset();
    m_HPWaterFluidNextFBO.reset();
    m_HPWaterFluidWaterHeightFBO.reset();
    m_HPWaterFluidSceneHeightFBO.reset();
    m_GBufferShader.reset();
    m_HPWaterGBufferShader.reset();
    m_HPWaterMaskShader.reset();
    m_HPWaterCompositeShader.reset();
    m_HPWaterVolumeShader.reset();
    m_HPWaterVolumeMotionVectorShader.reset();
    m_HPWaterVolumeTemporalShader.reset();
    m_HPWaterVolumeFilterShader.reset();
    m_HPWaterVolumeUpsampleShader.reset();
    m_HPWaterCausticShader.reset();
    m_HPWaterCausticFilterShader.reset();
    m_HPWaterCausticAtlasShader.reset();
    m_HPWaterCausticComputeShader.reset();
    m_HPWaterCausticResolveShader.reset();
    m_HPWaterCausticFilterComputeShader.reset();
    m_HPWaterFluidComputeShader.reset();
    m_HPWaterSpectrumComputeShader.reset();
    m_HPWaterDepthPyramidShader.reset();
    m_HPWaterDepthMergeShader.reset();
    m_HPWaterNormalMergeShader.reset();
    m_HPWaterFluidShader.reset();
    m_HPWaterFluidHeightCaptureShader.reset();
    m_LightingShader.reset();
    m_DebugShader.reset();
    m_HPWaterCompositeValid = false;
    m_HPWaterMaskValid = false;
    m_HPWaterVolumeValid = false;
    m_HPWaterVolumeTemporalValid = false;
    m_HPWaterVolumeHistoryValid = false;
    m_HPWaterVolumeFilteredValid = false;
    m_HPWaterVolumeUpsampledValid = false;
    m_HPWaterVolumeTemporalNeighborhoodClampEnabled = false;
    m_HPWaterVolumeTemporalMotionReprojectionEnabled = false;
    m_HPWaterVolumeExplicitMotionVectorEnabled = false;
    m_HPWaterVolumeSceneMotionVectorEnabled = false;
    m_HPWaterVolumeMotionVectorHistoryEnabled = false;
    m_HPWaterVolumeExponentialIntegrationEnabled = false;
    m_HPWaterVolumeShadowSamplingEnabled = false;
    m_HPWaterVolumeShadowParamsEnabled = false;
    m_HPWaterVolumePunctualLightLoopEnabled = false;
    m_HPWaterVolumeAreaLightLoopEnabled = false;
    m_HPWaterVolumePointLightCount = 0;
    m_HPWaterVolumeSpotLightCount = 0;
    m_HPWaterVolumeAreaLightCount = 0;
    m_HPWaterVolumeShadowSoftness = 0.0f;
    m_HPWaterVolumeShadowMinFilterSize = 0.0f;
    m_HPWaterVolumeShadowBlockerSamples = 0;
    m_HPWaterVolumeShadowFilterSamples = 0;
    m_HPWaterVolumeSampleCount = 0;
    m_HPWaterVolumeTemporalNeighborhoodClampStrength = 0.0f;
    m_HPWaterVolumeTemporalBlendFactor = 0.0f;
    m_HPWaterVolumeSpatialFilterEnabled = false;
    m_HPWaterVolumeSpatialFilterIterations = 0;
    m_HPWaterVolumeMotionVectorsEnabled = false;
    m_HPWaterVolumeMotionVectorVelocityScale = 0.0f;
    m_HPWaterVolumeTemporalDepthRejectionEnabled = false;
    m_HPWaterVolumeTemporalDepthThreshold = 0.0f;
    m_HPWaterVolumeSpatialDepthAwareEnabled = false;
    m_HPWaterVolumeSpatialDepthSensitivity = 0.0f;
    m_HPWaterCausticValid = false;
    m_HPWaterCausticFilteredValid = false;
    m_HPWaterCausticFilterComputeParityEnabled = false;
    m_HPWaterCausticFilterLDSHaloEnabled = false;
    m_HPWaterCausticComputeIrradianceValid = false;
    m_HPWaterCausticComputeIrradianceRan = false;
    m_HPWaterCausticComputeAtomicEnabled = false;
    m_HPWaterCausticShadowDepthConsumed = false;
    m_HPWaterCausticRGBReceiverProjectionEnabled = false;
    m_HPWaterCausticExponentialLightStepsEnabled = false;
    m_HPWaterCausticFrameDitherEnabled = false;
    m_HPWaterCausticAtlasReceiverOutputEnabled = false;
    m_HPWaterCausticCascadeBlendEnabled = false;
    m_HPWaterCausticAtlasEdgeFilterEnabled = false;
    m_HPWaterCausticSpectralWeightingEnabled = false;
    m_HPWaterFGDLUTValid = false;
    m_HPWaterCausticAtlasValid = false;
    m_HPWaterCausticAtlasConsumed = false;
    m_HPWaterCausticAtlasTileResolution = 0;
    m_HPWaterDepthPyramidValid = false;
    m_HPWaterDepthMergedToSceneDepth = false;
    m_HPWaterNormalMergedToSceneGBuffer = false;
    m_HPWaterStencilMarkedInSceneDepth = false;
    m_HPWaterStencilRef = 0;
    m_HPWaterFluidValid = false;
    m_HPWaterFluidInitialized = false;
    m_HPWaterFluidComputeRan = false;
    m_HPWaterFluidEdgeAbsorptionParityEnabled = false;
    m_HPWaterFluidSourceClampEnabled = false;
    m_HPWaterFluidObstacleValid = false;
    m_HPWaterFluidHeightFieldValid = false;
    m_HPWaterFluidHeightCaptureRan = false;
    m_HPWaterFluidHeightCaptureValid = false;
    m_HPWaterFluidHeightCaptureCacheReused = false;
    DestroyHPWaterSpectrumTexture();
    m_HPWaterFluidWaterHeightCaptured = false;
    m_HPWaterFluidSceneHeightCaptured = false;
    m_HPWaterFluidObstacleResolution = 0;
    m_HPWaterFluidHeightFieldResolution = 0;
    m_HPWaterFluidHeightCaptureResolution = 0;
    m_HPWaterFluidResolution = 0;
    m_HPWaterVolumeFilterIterations = 0;
    m_HPWaterCausticFilterIterations = 0;

    if (m_QuadVAO) {
        VE_GPU_UNTRACK(GPUResourceType::VertexArray, m_QuadVAO);
        glDeleteVertexArrays(1, &m_QuadVAO);
        m_QuadVAO = 0;
    }

    m_Initialized = false;
}

void DeferredRenderer::CreateLightingFBO() {
    FramebufferSpec lightSpec;
    lightSpec.Width = m_Width;
    lightSpec.Height = m_Height;
    lightSpec.HDR = true; // RGBA16F for HDR output
    m_LightingFBO = Framebuffer::Create(lightSpec);
}

void DeferredRenderer::CreateHPWaterCompositeFBO() {
    FramebufferSpec compositeSpec;
    compositeSpec.Width = m_Width;
    compositeSpec.Height = m_Height;
    compositeSpec.ColorFormats = {
        { GL_RGBA16F }, // RT0: Final HPWater composite color
        { GL_RGBA16F }, // RT1: Refracted world position.xyz + ray length
        { GL_RGBA16F }, // RT2: Refract UV.xy + scene depth + normalized thickness
        { GL_RGBA16F }, // RT3: SSR confidence, hit mask, hierarchy weight, enabled strength
    };
    m_HPWaterCompositeFBO = Framebuffer::Create(compositeSpec);
    m_HPWaterCompositeValid = false;
    m_HPWaterSurfaceShadowSamplingEnabled = false;
    m_HPWaterShadowCascadeDitherEnabled = false;
}

void DeferredRenderer::CreateHPWaterCausticFBO() {
    FramebufferSpec causticSpec;
    causticSpec.Width = m_Width;
    causticSpec.Height = m_Height;
    causticSpec.ColorFormats = {
        { GL_RGBA16F }, // RT0: caustic rgb energy + scalar weight
    };
    m_HPWaterCausticFBO = Framebuffer::Create(causticSpec);
    m_HPWaterCausticFilteredFBO = Framebuffer::Create(causticSpec);
    m_HPWaterCausticFilterScratchFBO = Framebuffer::Create(causticSpec);
    m_HPWaterCausticValid = false;
    m_HPWaterCausticFilteredValid = false;
    m_HPWaterCausticFilterIterations = 0;
}

void DeferredRenderer::DestroyHPWaterCausticComputeTexture() {
    if (m_HPWaterCausticComputeIrradianceTexture != 0) {
        VE_GPU_UNTRACK(GPUResourceType::Texture, m_HPWaterCausticComputeIrradianceTexture);
        glDeleteTextures(1, &m_HPWaterCausticComputeIrradianceTexture);
        m_HPWaterCausticComputeIrradianceTexture = 0;
    }
    for (uint32_t& texture : m_HPWaterCausticComputeAtomicTextures) {
        if (texture != 0) {
            VE_GPU_UNTRACK(GPUResourceType::Texture, texture);
            glDeleteTextures(1, &texture);
            texture = 0;
        }
    }
    m_HPWaterCausticComputeWidth = 0;
    m_HPWaterCausticComputeHeight = 0;
    m_HPWaterCausticComputeIrradianceValid = false;
    m_HPWaterCausticComputeIrradianceRan = false;
    m_HPWaterCausticComputeAtomicEnabled = false;
    m_HPWaterCausticShadowDepthConsumed = false;
    m_HPWaterCausticRGBReceiverProjectionEnabled = false;
    m_HPWaterCausticExponentialLightStepsEnabled = false;
    m_HPWaterCausticFrameDitherEnabled = false;
    m_HPWaterCausticAtlasReceiverOutputEnabled = false;
    m_HPWaterCausticCascadeBlendEnabled = false;
    m_HPWaterCausticAtlasEdgeFilterEnabled = false;
    m_HPWaterCausticSpectralWeightingEnabled = false;
}

void DeferredRenderer::CreateHPWaterCausticComputeTexture(uint32_t width, uint32_t height) {
    DestroyHPWaterCausticComputeTexture();
    if (width == 0)
        width = m_Width;
    if (height == 0)
        height = m_Height;
    if (width == 0 || height == 0)
        return;

    m_HPWaterCausticComputeWidth = width;
    m_HPWaterCausticComputeHeight = height;

    glGenTextures(1, &m_HPWaterCausticComputeIrradianceTexture);
    VE_GPU_TRACK(GPUResourceType::Texture, m_HPWaterCausticComputeIrradianceTexture);
    glBindTexture(GL_TEXTURE_2D, m_HPWaterCausticComputeIrradianceTexture);
    glTexStorage2D(GL_TEXTURE_2D,
                   1,
                   GL_RGBA16F,
                   static_cast<GLsizei>(m_HPWaterCausticComputeWidth),
                   static_cast<GLsizei>(m_HPWaterCausticComputeHeight));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenTextures(4, m_HPWaterCausticComputeAtomicTextures);
    for (uint32_t texture : m_HPWaterCausticComputeAtomicTextures) {
        VE_GPU_TRACK(GPUResourceType::Texture, texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexStorage2D(GL_TEXTURE_2D,
                       1,
                       GL_R32UI,
                       static_cast<GLsizei>(m_HPWaterCausticComputeWidth),
                       static_cast<GLsizei>(m_HPWaterCausticComputeHeight));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    m_HPWaterCausticComputeIrradianceValid = false;
    m_HPWaterCausticComputeIrradianceRan = false;
    m_HPWaterCausticComputeAtomicEnabled = false;
    m_HPWaterCausticShadowDepthConsumed = false;
    m_HPWaterCausticRGBReceiverProjectionEnabled = false;
    m_HPWaterCausticExponentialLightStepsEnabled = false;
    m_HPWaterCausticFrameDitherEnabled = false;
    m_HPWaterCausticAtlasReceiverOutputEnabled = false;
    m_HPWaterCausticCascadeBlendEnabled = false;
    m_HPWaterCausticAtlasEdgeFilterEnabled = false;
    m_HPWaterCausticSpectralWeightingEnabled = false;
}

void DeferredRenderer::DestroyHPWaterFGDLUT() {
    if (m_HPWaterFGDLUTTexture != 0) {
        VE_GPU_UNTRACK(GPUResourceType::Texture, m_HPWaterFGDLUTTexture);
        glDeleteTextures(1, &m_HPWaterFGDLUTTexture);
        m_HPWaterFGDLUTTexture = 0;
    }
    m_HPWaterFGDLUTValid = false;
}

void DeferredRenderer::CreateHPWaterFGDLUT() {
    DestroyHPWaterFGDLUT();

    const uint32_t resolution = std::clamp(m_HPWaterFGDLUTResolution, 32u, 512u);
    m_HPWaterFGDLUTResolution = resolution;

    std::vector<float> pixels(static_cast<size_t>(resolution) * static_cast<size_t>(resolution) * 2u);
    for (uint32_t y = 0; y < resolution; ++y) {
        const float roughness = (static_cast<float>(y) + 0.5f) / static_cast<float>(resolution);
        for (uint32_t x = 0; x < resolution; ++x) {
            const float nDotV = (static_cast<float>(x) + 0.5f) / static_cast<float>(resolution);
            const glm::vec2 brdf = IntegrateBRDF(nDotV, roughness);
            const size_t index = (static_cast<size_t>(y) * resolution + x) * 2u;
            pixels[index + 0u] = brdf.x;
            pixels[index + 1u] = brdf.y;
        }
    }

    glGenTextures(1, &m_HPWaterFGDLUTTexture);
    VE_GPU_TRACK(GPUResourceType::Texture, m_HPWaterFGDLUTTexture);
    glBindTexture(GL_TEXTURE_2D, m_HPWaterFGDLUTTexture);
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_RG16F,
                 static_cast<GLsizei>(resolution),
                 static_cast<GLsizei>(resolution),
                 0,
                 GL_RG,
                 GL_FLOAT,
                 pixels.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    m_HPWaterFGDLUTValid = true;
}

void DeferredRenderer::CreateHPWaterCausticAtlasFBO(uint32_t tileResolution) {
    tileResolution = std::clamp(tileResolution, 128u, 2048u);
    if (m_HPWaterCausticAtlasFBO && m_HPWaterCausticAtlasTileResolution == tileResolution)
        return;

    FramebufferSpec atlasSpec;
    atlasSpec.Width = tileResolution * 2u;
    atlasSpec.Height = tileResolution * 2u;
    atlasSpec.ColorFormats = {
        { GL_RGBA16F }, // RT0: encoded normal.xyz + normalized thickness/roughness
    };
    m_HPWaterCausticAtlasFBO = Framebuffer::Create(atlasSpec);
    m_HPWaterCausticAtlasTileResolution = tileResolution;
    m_HPWaterCausticAtlasValid = false;
}

void DeferredRenderer::DestroyHPWaterDepthPyramid() {
    if (m_HPWaterDepthPyramidTexture != 0) {
        VE_GPU_UNTRACK(GPUResourceType::Texture, m_HPWaterDepthPyramidTexture);
        glDeleteTextures(1, &m_HPWaterDepthPyramidTexture);
        m_HPWaterDepthPyramidTexture = 0;
    }

    if (m_HPWaterDepthPyramidFBO != 0) {
        VE_GPU_UNTRACK(GPUResourceType::Framebuffer, m_HPWaterDepthPyramidFBO);
        glDeleteFramebuffers(1, &m_HPWaterDepthPyramidFBO);
        m_HPWaterDepthPyramidFBO = 0;
    }

    m_HPWaterDepthPyramidMipCount = 0;
    m_HPWaterDepthPyramidValid = false;
}

void DeferredRenderer::CreateHPWaterDepthPyramid() {
    DestroyHPWaterDepthPyramid();

    if (m_Width == 0 || m_Height == 0)
        return;

    uint32_t maxDim = std::max(m_Width, m_Height);
    m_HPWaterDepthPyramidMipCount = 1;
    while (maxDim > 1) {
        maxDim /= 2u;
        ++m_HPWaterDepthPyramidMipCount;
    }

    glGenTextures(1, &m_HPWaterDepthPyramidTexture);
    VE_GPU_TRACK(GPUResourceType::Texture, m_HPWaterDepthPyramidTexture);
    glBindTexture(GL_TEXTURE_2D, m_HPWaterDepthPyramidTexture);
    glTexStorage2D(GL_TEXTURE_2D,
                   static_cast<GLsizei>(m_HPWaterDepthPyramidMipCount),
                   GL_R32F,
                   static_cast<GLsizei>(m_Width),
                   static_cast<GLsizei>(m_Height));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(m_HPWaterDepthPyramidMipCount - 1u));
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &m_HPWaterDepthPyramidFBO);
    VE_GPU_TRACK(GPUResourceType::Framebuffer, m_HPWaterDepthPyramidFBO);
    m_HPWaterDepthPyramidValid = false;
}

uint32_t DeferredRenderer::GetHalfResolution(uint32_t value) {
    return std::max(1u, (value + 1u) / 2u);
}

void DeferredRenderer::CreateHPWaterVolumeFBO() {
    FramebufferSpec volumeSpec;
    volumeSpec.Width = GetHalfResolution(m_Width);
    volumeSpec.Height = GetHalfResolution(m_Height);
    volumeSpec.ColorFormats = {
        { GL_RGBA16F }, // RT0: low-res volumetric in-scattering color
        { GL_RGBA16F }, // RT1: low-res transmittance
        { GL_RGBA16F }, // RT2: refracted linear depth + thickness diagnostics
    };
    m_HPWaterVolumeFBO = Framebuffer::Create(volumeSpec);

    FramebufferSpec motionSpec;
    motionSpec.Width = volumeSpec.Width;
    motionSpec.Height = volumeSpec.Height;
    motionSpec.ColorFormats = {
        { GL_RG16F }, // RT0: previousUV - currentUV for volume temporal rejection
    };
    m_HPWaterVolumeMotionVectorFBO = Framebuffer::Create(motionSpec);

    m_HPWaterVolumeTemporalFBO = Framebuffer::Create(volumeSpec);
    m_HPWaterVolumeHistoryFBO = Framebuffer::Create(volumeSpec);
    m_HPWaterVolumeFilteredFBO = Framebuffer::Create(volumeSpec);
    m_HPWaterVolumeFilterScratchFBO = Framebuffer::Create(volumeSpec);

    FramebufferSpec upsampleSpec;
    upsampleSpec.Width = m_Width;
    upsampleSpec.Height = m_Height;
    upsampleSpec.ColorFormats = {
        { GL_RGBA16F }, // RT0: full-resolution volumetric in-scattering color
        { GL_RGBA16F }, // RT1: full-resolution transmittance
        { GL_RGBA16F }, // RT2: full-resolution refracted linear depth + diagnostics
    };
    m_HPWaterVolumeUpsampledFBO = Framebuffer::Create(upsampleSpec);

    m_HPWaterVolumeValid = false;
    m_HPWaterVolumeTemporalValid = false;
    m_HPWaterVolumeHistoryValid = false;
    m_HPWaterVolumeFilteredValid = false;
    m_HPWaterVolumeUpsampledValid = false;
    m_HPWaterVolumeTemporalNeighborhoodClampEnabled = false;
    m_HPWaterVolumeTemporalMotionReprojectionEnabled = false;
    m_HPWaterVolumeExplicitMotionVectorEnabled = false;
    m_HPWaterVolumeSceneMotionVectorEnabled = false;
    m_HPWaterVolumeMotionVectorHistoryEnabled = false;
    m_HPWaterVolumeExponentialIntegrationEnabled = false;
    m_HPWaterVolumeShadowSamplingEnabled = false;
    m_HPWaterVolumeShadowParamsEnabled = false;
    m_HPWaterVolumePunctualLightLoopEnabled = false;
    m_HPWaterVolumeAreaLightLoopEnabled = false;
    m_HPWaterVolumePointLightCount = 0;
    m_HPWaterVolumeSpotLightCount = 0;
    m_HPWaterVolumeAreaLightCount = 0;
    m_HPWaterVolumeShadowSoftness = 0.0f;
    m_HPWaterVolumeShadowMinFilterSize = 0.0f;
    m_HPWaterVolumeShadowBlockerSamples = 0;
    m_HPWaterVolumeShadowFilterSamples = 0;
    m_HPWaterVolumeSampleCount = 0;
    m_HPWaterVolumeTemporalNeighborhoodClampStrength = 0.0f;
    m_HPWaterVolumeTemporalBlendFactor = 0.0f;
    m_HPWaterVolumeSpatialFilterEnabled = false;
    m_HPWaterVolumeSpatialFilterIterations = 0;
    m_HPWaterVolumeMotionVectorsEnabled = false;
    m_HPWaterVolumeMotionVectorVelocityScale = 0.0f;
    m_HPWaterVolumeTemporalDepthRejectionEnabled = false;
    m_HPWaterVolumeTemporalDepthThreshold = 0.0f;
    m_HPWaterVolumeSpatialDepthAwareEnabled = false;
    m_HPWaterVolumeSpatialDepthSensitivity = 0.0f;
    m_HPWaterVolumeFilterIterations = 0;
}

void DeferredRenderer::CreateHPWaterFluidFBO(uint32_t resolution) {
    resolution = std::clamp(resolution, 16u, 1024u);
    if (m_HPWaterFluidCurrentFBO && m_HPWaterFluidPreviousFBO && m_HPWaterFluidNextFBO &&
        m_HPWaterFluidResolution == resolution) {
        return;
    }

    FramebufferSpec fluidSpec;
    fluidSpec.Width = resolution;
    fluidSpec.Height = resolution;
    fluidSpec.ColorFormats = {
        { GL_R16F },
    };
    m_HPWaterFluidCurrentFBO = Framebuffer::Create(fluidSpec);
    m_HPWaterFluidPreviousFBO = Framebuffer::Create(fluidSpec);
    m_HPWaterFluidNextFBO = Framebuffer::Create(fluidSpec);
    m_HPWaterFluidResolution = resolution;
    ClearHPWaterFluidFBOs();
}

void DeferredRenderer::CreateHPWaterFluidHeightCaptureFBO(uint32_t resolution) {
    resolution = std::clamp(resolution, 16u, 1024u);
    if (m_HPWaterFluidWaterHeightFBO &&
        m_HPWaterFluidSceneHeightFBO &&
        m_HPWaterFluidHeightCaptureResolution == resolution) {
        return;
    }

    FramebufferSpec heightSpec;
    heightSpec.Width = resolution;
    heightSpec.Height = resolution;
    heightSpec.ColorFormats = {
        { GL_R16F },
    };
    m_HPWaterFluidWaterHeightFBO = Framebuffer::Create(heightSpec);
    m_HPWaterFluidSceneHeightFBO = Framebuffer::Create(heightSpec);
    m_HPWaterFluidHeightCaptureResolution = resolution;
    m_HPWaterFluidHeightCaptureValid = false;
    m_HPWaterFluidWaterHeightCaptured = false;
    m_HPWaterFluidSceneHeightCaptured = false;
}

void DeferredRenderer::DestroyHPWaterFluidObstacleTexture() {
    if (m_HPWaterFluidObstacleTexture != 0) {
        VE_GPU_UNTRACK(GPUResourceType::Texture, m_HPWaterFluidObstacleTexture);
        glDeleteTextures(1, &m_HPWaterFluidObstacleTexture);
        m_HPWaterFluidObstacleTexture = 0;
    }
    m_HPWaterFluidObstacleResolution = 0;
    m_HPWaterFluidObstacleValid = false;
}

void DeferredRenderer::DestroyHPWaterFluidHeightFieldTextures() {
    if (m_HPWaterFluidWaterHeightTexture != 0) {
        VE_GPU_UNTRACK(GPUResourceType::Texture, m_HPWaterFluidWaterHeightTexture);
        glDeleteTextures(1, &m_HPWaterFluidWaterHeightTexture);
        m_HPWaterFluidWaterHeightTexture = 0;
    }
    if (m_HPWaterFluidSceneHeightTexture != 0) {
        VE_GPU_UNTRACK(GPUResourceType::Texture, m_HPWaterFluidSceneHeightTexture);
        glDeleteTextures(1, &m_HPWaterFluidSceneHeightTexture);
        m_HPWaterFluidSceneHeightTexture = 0;
    }
    m_HPWaterFluidHeightFieldResolution = 0;
    m_HPWaterFluidHeightFieldValid = m_HPWaterFluidHeightCaptureValid;
}

void DeferredRenderer::BeginHPWaterFluidHeightCaptureFrame(bool startFrameBakeEnabled) {
    m_HPWaterFluidHeightCaptureRan = false;
    m_HPWaterFluidHeightCaptureCacheReused = false;
    if (!startFrameBakeEnabled) {
        m_HPWaterFluidHeightCaptureValid = false;
        m_HPWaterFluidWaterHeightCaptured = false;
        m_HPWaterFluidSceneHeightCaptured = false;
    }
}

bool DeferredRenderer::CanReuseHPWaterFluidHeightCapture(uint32_t resolution,
                                                         const glm::vec3& boxCenter,
                                                         const glm::vec3& boxSize) const {
    resolution = std::clamp(resolution, 16u, 1024u);
    if (!m_HPWaterFluidWaterHeightFBO || !m_HPWaterFluidSceneHeightFBO)
        return false;
    if (!m_HPWaterFluidHeightCaptureValid ||
        !m_HPWaterFluidWaterHeightCaptured ||
        !m_HPWaterFluidSceneHeightCaptured)
        return false;
    if (m_HPWaterFluidHeightCaptureResolution != resolution)
        return false;

    const glm::vec3 safeBoxSize = glm::max(boxSize, glm::vec3(0.001f));
    return glm::length(m_HPWaterFluidBoxCenter - boxCenter) < 0.0001f &&
           glm::length(m_HPWaterFluidBoxSize - safeBoxSize) < 0.0001f;
}

void DeferredRenderer::MarkHPWaterFluidHeightCaptureCacheReused() {
    m_HPWaterFluidHeightCaptureRan = false;
    m_HPWaterFluidHeightCaptureCacheReused = true;
    m_HPWaterFluidHeightFieldValid = m_HPWaterFluidHeightCaptureValid || m_HPWaterFluidHeightFieldValid;
}

bool DeferredRenderer::BeginHPWaterFluidHeightCaptureTarget(const std::shared_ptr<Framebuffer>& target,
                                                            uint32_t resolution,
                                                            const glm::vec3& boxCenter,
                                                            const glm::vec3& boxSize) {
    if (!m_HPWaterFluidHeightCaptureShader)
        return false;

    resolution = std::clamp(resolution, 16u, 1024u);
    CreateHPWaterFluidHeightCaptureFBO(resolution);
    if (!target)
        return false;

    target->Bind();
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, static_cast<GLsizei>(resolution), static_cast<GLsizei>(resolution));
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_MAX);
    glBlendFunc(GL_ONE, GL_ONE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    m_HPWaterFluidHeightCaptureRan = true;
    m_HPWaterFluidBoxCenter = boxCenter;
    m_HPWaterFluidBoxSize = glm::max(boxSize, glm::vec3(0.001f));
    return true;
}

void DeferredRenderer::EndHPWaterFluidHeightCaptureTarget() {
    glBlendEquation(GL_FUNC_ADD);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool DeferredRenderer::BeginHPWaterFluidWaterHeightCapture(uint32_t resolution,
                                                           const glm::vec3& boxCenter,
                                                           const glm::vec3& boxSize) {
    CreateHPWaterFluidHeightCaptureFBO(resolution);
    m_HPWaterFluidWaterHeightCaptured = false;
    m_HPWaterFluidHeightCaptureValid = false;
    return BeginHPWaterFluidHeightCaptureTarget(m_HPWaterFluidWaterHeightFBO,
                                                resolution,
                                                boxCenter,
                                                boxSize);
}

bool DeferredRenderer::BeginHPWaterFluidSceneHeightCapture(uint32_t resolution,
                                                           const glm::vec3& boxCenter,
                                                           const glm::vec3& boxSize) {
    CreateHPWaterFluidHeightCaptureFBO(resolution);
    m_HPWaterFluidSceneHeightCaptured = false;
    m_HPWaterFluidHeightCaptureValid = false;
    return BeginHPWaterFluidHeightCaptureTarget(m_HPWaterFluidSceneHeightFBO,
                                                resolution,
                                                boxCenter,
                                                boxSize);
}

void DeferredRenderer::EndHPWaterFluidWaterHeightCapture(bool valid) {
    EndHPWaterFluidHeightCaptureTarget();
    m_HPWaterFluidWaterHeightCaptured = valid;
    m_HPWaterFluidHeightCaptureValid = m_HPWaterFluidWaterHeightCaptured && m_HPWaterFluidSceneHeightCaptured;
    m_HPWaterFluidHeightFieldValid = m_HPWaterFluidHeightCaptureValid || m_HPWaterFluidHeightFieldValid;
}

void DeferredRenderer::EndHPWaterFluidSceneHeightCapture(bool valid) {
    EndHPWaterFluidHeightCaptureTarget();
    m_HPWaterFluidSceneHeightCaptured = valid;
    m_HPWaterFluidHeightCaptureValid = m_HPWaterFluidWaterHeightCaptured && m_HPWaterFluidSceneHeightCaptured;
    m_HPWaterFluidHeightFieldValid = m_HPWaterFluidHeightCaptureValid || m_HPWaterFluidHeightFieldValid;
}

bool DeferredRenderer::UploadHPWaterFluidObstacleMask(uint32_t resolution,
                                                      const std::vector<uint8_t>& maskPixels,
                                                      const glm::vec3& boxCenter,
                                                      const glm::vec3& boxSize) {
    resolution = std::clamp(resolution, 16u, 1024u);
    const size_t expectedSize = static_cast<size_t>(resolution) * static_cast<size_t>(resolution);
    if (maskPixels.size() != expectedSize) {
        m_HPWaterFluidObstacleValid = false;
        return false;
    }

    if (m_HPWaterFluidObstacleTexture == 0 || m_HPWaterFluidObstacleResolution != resolution) {
        DestroyHPWaterFluidObstacleTexture();
        glGenTextures(1, &m_HPWaterFluidObstacleTexture);
        VE_GPU_TRACK(GPUResourceType::Texture, m_HPWaterFluidObstacleTexture);
        glBindTexture(GL_TEXTURE_2D, m_HPWaterFluidObstacleTexture);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8,
                       static_cast<GLsizei>(resolution),
                       static_cast<GLsizei>(resolution));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        m_HPWaterFluidObstacleResolution = resolution;
    } else {
        glBindTexture(GL_TEXTURE_2D, m_HPWaterFluidObstacleTexture);
    }

    GLint previousUnpackAlignment = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &previousUnpackAlignment);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D,
                    0,
                    0,
                    0,
                    static_cast<GLsizei>(resolution),
                    static_cast<GLsizei>(resolution),
                    GL_RED,
                    GL_UNSIGNED_BYTE,
                    maskPixels.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, previousUnpackAlignment);
    glBindTexture(GL_TEXTURE_2D, 0);

    m_HPWaterFluidObstacleValid = true;
    m_HPWaterFluidBoxCenter = boxCenter;
    m_HPWaterFluidBoxSize = glm::max(boxSize, glm::vec3(0.001f));
    return true;
}

bool DeferredRenderer::UploadHPWaterFluidHeightFields(uint32_t resolution,
                                                      const std::vector<float>& waterHeights,
                                                      const std::vector<float>& sceneHeights,
                                                      const glm::vec3& boxCenter,
                                                      const glm::vec3& boxSize) {
    resolution = std::clamp(resolution, 16u, 1024u);
    const size_t expectedSize = static_cast<size_t>(resolution) * static_cast<size_t>(resolution);
    if (waterHeights.size() != expectedSize || sceneHeights.size() != expectedSize) {
        m_HPWaterFluidHeightFieldValid = false;
        return false;
    }

    if (m_HPWaterFluidWaterHeightTexture == 0 ||
        m_HPWaterFluidSceneHeightTexture == 0 ||
        m_HPWaterFluidHeightFieldResolution != resolution) {
        DestroyHPWaterFluidHeightFieldTextures();

        GLuint textures[2] = {};
        glGenTextures(2, textures);
        m_HPWaterFluidWaterHeightTexture = textures[0];
        m_HPWaterFluidSceneHeightTexture = textures[1];
        VE_GPU_TRACK(GPUResourceType::Texture, m_HPWaterFluidWaterHeightTexture);
        VE_GPU_TRACK(GPUResourceType::Texture, m_HPWaterFluidSceneHeightTexture);

        for (GLuint texture : textures) {
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexStorage2D(GL_TEXTURE_2D,
                           1,
                           GL_R16F,
                           static_cast<GLsizei>(resolution),
                           static_cast<GLsizei>(resolution));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        m_HPWaterFluidHeightFieldResolution = resolution;
    }

    GLint previousUnpackAlignment = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &previousUnpackAlignment);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glBindTexture(GL_TEXTURE_2D, m_HPWaterFluidWaterHeightTexture);
    glTexSubImage2D(GL_TEXTURE_2D,
                    0,
                    0,
                    0,
                    static_cast<GLsizei>(resolution),
                    static_cast<GLsizei>(resolution),
                    GL_RED,
                    GL_FLOAT,
                    waterHeights.data());

    glBindTexture(GL_TEXTURE_2D, m_HPWaterFluidSceneHeightTexture);
    glTexSubImage2D(GL_TEXTURE_2D,
                    0,
                    0,
                    0,
                    static_cast<GLsizei>(resolution),
                    static_cast<GLsizei>(resolution),
                    GL_RED,
                    GL_FLOAT,
                    sceneHeights.data());

    glPixelStorei(GL_UNPACK_ALIGNMENT, previousUnpackAlignment);
    glBindTexture(GL_TEXTURE_2D, 0);

    m_HPWaterFluidHeightFieldValid = true;
    m_HPWaterFluidBoxCenter = boxCenter;
    m_HPWaterFluidBoxSize = glm::max(boxSize, glm::vec3(0.001f));
    return true;
}

void DeferredRenderer::ClearHPWaterFluidFBOs() {
    const std::shared_ptr<Framebuffer> targets[] = {
        m_HPWaterFluidCurrentFBO,
        m_HPWaterFluidPreviousFBO,
        m_HPWaterFluidNextFBO,
    };

    GLboolean previousDepthMask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &previousDepthMask);
    const bool previousDepthTest = glIsEnabled(GL_DEPTH_TEST) == GL_TRUE;
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

    for (const auto& fbo : targets) {
        if (!fbo)
            continue;
        fbo->Bind();
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        fbo->Unbind();
    }

    if (previousDepthTest)
        glEnable(GL_DEPTH_TEST);
    else
        glDisable(GL_DEPTH_TEST);
    glDepthMask(previousDepthMask);

    m_HPWaterFluidValid = false;
    m_HPWaterFluidComputeRan = false;
    m_HPWaterFluidEdgeAbsorptionParityEnabled = false;
    m_HPWaterFluidSourceClampEnabled = false;
    m_HPWaterFluidInitialized = true;
}

void DeferredRenderer::CreateHPWaterSpectrumTexture(uint32_t resolution) {
    resolution = std::clamp(resolution, 16u, 2048u);
    if (m_HPWaterSpectrumTexture != 0 && m_HPWaterSpectrumResolution == resolution)
        return;

    DestroyHPWaterSpectrumTexture();

    glGenTextures(1, &m_HPWaterSpectrumTexture);
    VE_GPU_TRACK(GPUResourceType::Texture, m_HPWaterSpectrumTexture);
    glBindTexture(GL_TEXTURE_2D, m_HPWaterSpectrumTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F,
                 static_cast<GLsizei>(resolution),
                 static_cast<GLsizei>(resolution),
                 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    m_HPWaterSpectrumResolution = resolution;
    m_HPWaterSpectrumComputeValid = false;
}

void DeferredRenderer::DestroyHPWaterSpectrumTexture() {
    if (m_HPWaterSpectrumTexture != 0) {
        VE_GPU_UNTRACK(GPUResourceType::Texture, m_HPWaterSpectrumTexture);
        GLuint texture = static_cast<GLuint>(m_HPWaterSpectrumTexture);
        glDeleteTextures(1, &texture);
    }
    m_HPWaterSpectrumTexture = 0;
    m_HPWaterSpectrumResolution = 0;
    m_HPWaterSpectrumComputeValid = false;
    m_HPWaterSpectrumComputeRan = false;
}

void DeferredRenderer::CreateHPWaterGBuffer() {
    FramebufferSpec waterSpec;
    waterSpec.Width = m_Width;
    waterSpec.Height = m_Height;
    waterSpec.ColorFormats = {
        { GL_RGBA16F }, // RT0: Water normal + roughness
        { GL_RGBA16F }, // RT1: Scatter color + thickness
        { GL_RGBA16F }, // RT2: Absorption color + foam
    };
    m_HPWaterGBuffer = Framebuffer::Create(waterSpec);
}

void DeferredRenderer::CreateHPWaterMaskFBO() {
    FramebufferSpec maskSpec;
    maskSpec.Width = m_Width;
    maskSpec.Height = m_Height;
    maskSpec.ColorFormats = {
        { GL_R8 }, // RT0: explicit HPWater coverage mask
    };
    m_HPWaterMaskFBO = Framebuffer::Create(maskSpec);
    m_HPWaterMaskValid = false;
}

void DeferredRenderer::Resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0 || width > 8192 || height > 8192) return;
    if (width == m_Width && height == m_Height) return;

    m_Width = width;
    m_Height = height;

    if (m_GBuffer)
        m_GBuffer->Resize(width, height);

    if (m_LightingFBO)
        m_LightingFBO->Resize(width, height);

    if (m_HPWaterGBuffer)
        m_HPWaterGBuffer->Resize(width, height);

    if (m_HPWaterMaskFBO)
        m_HPWaterMaskFBO->Resize(width, height);

    if (m_HPWaterCompositeFBO)
        m_HPWaterCompositeFBO->Resize(width, height);

    if (m_HPWaterVolumeFBO)
        m_HPWaterVolumeFBO->Resize(GetHalfResolution(width), GetHalfResolution(height));

    if (m_HPWaterVolumeMotionVectorFBO)
        m_HPWaterVolumeMotionVectorFBO->Resize(GetHalfResolution(width), GetHalfResolution(height));

    if (m_HPWaterVolumeTemporalFBO)
        m_HPWaterVolumeTemporalFBO->Resize(GetHalfResolution(width), GetHalfResolution(height));

    if (m_HPWaterVolumeHistoryFBO)
        m_HPWaterVolumeHistoryFBO->Resize(GetHalfResolution(width), GetHalfResolution(height));

    if (m_HPWaterVolumeFilteredFBO)
        m_HPWaterVolumeFilteredFBO->Resize(GetHalfResolution(width), GetHalfResolution(height));

    if (m_HPWaterVolumeFilterScratchFBO)
        m_HPWaterVolumeFilterScratchFBO->Resize(GetHalfResolution(width), GetHalfResolution(height));

    if (m_HPWaterVolumeUpsampledFBO)
        m_HPWaterVolumeUpsampledFBO->Resize(width, height);

    if (m_HPWaterCausticFBO)
        m_HPWaterCausticFBO->Resize(width, height);
    if (m_HPWaterCausticFilteredFBO)
        m_HPWaterCausticFilteredFBO->Resize(width, height);
    if (m_HPWaterCausticFilterScratchFBO)
        m_HPWaterCausticFilterScratchFBO->Resize(width, height);

    CreateHPWaterCausticComputeTexture();
    CreateHPWaterDepthPyramid();
    if (m_HPWaterFluidResolution > 0)
        CreateHPWaterFluidFBO(m_HPWaterFluidResolution);

    m_HPWaterCompositeValid = false;
    m_HPWaterMaskValid = false;
    m_HPWaterVolumeValid = false;
    m_HPWaterVolumeTemporalValid = false;
    m_HPWaterVolumeHistoryValid = false;
    m_HPWaterVolumeFilteredValid = false;
    m_HPWaterVolumeUpsampledValid = false;
    m_HPWaterVolumeExplicitMotionVectorEnabled = false;
    m_HPWaterVolumeSceneMotionVectorEnabled = false;
    m_HPWaterVolumeMotionVectorHistoryEnabled = false;
    m_HPWaterVolumeExponentialIntegrationEnabled = false;
    m_HPWaterVolumeShadowSamplingEnabled = false;
    m_HPWaterVolumeShadowParamsEnabled = false;
    m_HPWaterVolumePunctualLightLoopEnabled = false;
    m_HPWaterVolumeAreaLightLoopEnabled = false;
    m_HPWaterVolumePointLightCount = 0;
    m_HPWaterVolumeSpotLightCount = 0;
    m_HPWaterVolumeAreaLightCount = 0;
    m_HPWaterVolumeShadowSoftness = 0.0f;
    m_HPWaterVolumeShadowMinFilterSize = 0.0f;
    m_HPWaterVolumeShadowBlockerSamples = 0;
    m_HPWaterVolumeShadowFilterSamples = 0;
    m_HPWaterVolumeSampleCount = 0;
    m_HPWaterVolumeTemporalBlendFactor = 0.0f;
    m_HPWaterVolumeSpatialFilterEnabled = false;
    m_HPWaterVolumeSpatialFilterIterations = 0;
    m_HPWaterVolumeMotionVectorsEnabled = false;
    m_HPWaterVolumeMotionVectorVelocityScale = 0.0f;
    m_HPWaterVolumeTemporalDepthRejectionEnabled = false;
    m_HPWaterVolumeTemporalDepthThreshold = 0.0f;
    m_HPWaterVolumeSpatialDepthAwareEnabled = false;
    m_HPWaterVolumeSpatialDepthSensitivity = 0.0f;
    m_HPWaterCausticValid = false;
    m_HPWaterCausticFilteredValid = false;
    m_HPWaterCausticComputeIrradianceValid = false;
    m_HPWaterCausticComputeIrradianceRan = false;
    m_HPWaterCausticAtlasConsumed = false;
    m_HPWaterCausticShadowDepthConsumed = false;
    m_HPWaterCausticRGBReceiverProjectionEnabled = false;
    m_HPWaterCausticExponentialLightStepsEnabled = false;
    m_HPWaterCausticFrameDitherEnabled = false;
    m_HPWaterCausticAtlasReceiverOutputEnabled = false;
    m_HPWaterCausticCascadeBlendEnabled = false;
    m_HPWaterCausticAtlasEdgeFilterEnabled = false;
    m_HPWaterCausticSpectralWeightingEnabled = false;
    m_HPWaterDepthPyramidValid = false;
    m_HPWaterDepthMergedToSceneDepth = false;
    m_HPWaterNormalMergedToSceneGBuffer = false;
    m_HPWaterStencilMarkedInSceneDepth = false;
    m_HPWaterStencilRef = 0;
    m_HPWaterVolumeFilterIterations = 0;
    m_HPWaterCausticFilterIterations = 0;
}

void DeferredRenderer::ClearHPWaterGBuffer() {
    if (!m_HPWaterGBuffer) return;

    m_HPWaterGBuffer->Bind();
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glStencilMask(0xFF);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    m_HPWaterMaskValid = false;
    m_HPWaterCausticAtlasValid = false;
    m_HPWaterCausticAtlasConsumed = false;
    m_HPWaterCausticComputeIrradianceValid = false;
    m_HPWaterCausticComputeIrradianceRan = false;
    m_HPWaterCausticShadowDepthConsumed = false;
    m_HPWaterCausticRGBReceiverProjectionEnabled = false;
    m_HPWaterCausticExponentialLightStepsEnabled = false;
    m_HPWaterCausticFrameDitherEnabled = false;
    m_HPWaterCausticAtlasReceiverOutputEnabled = false;
    m_HPWaterCausticCascadeBlendEnabled = false;
    m_HPWaterCausticAtlasEdgeFilterEnabled = false;
    m_HPWaterCausticSpectralWeightingEnabled = false;
}

void DeferredRenderer::BeginGeometryPass() {
    if (!m_GBuffer) return;

    ClearHPWaterGBuffer();
    m_HPWaterDepthMergedToSceneDepth = false;
    m_HPWaterNormalMergedToSceneGBuffer = false;
    m_HPWaterStencilMarkedInSceneDepth = false;
    m_HPWaterStencilRef = 0;

    m_GBuffer->Bind();

    // Clear all G-buffer attachments to zero
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glStencilMask(0xFF);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glDisable(GL_STENCIL_TEST);

    // Standard opaque render state
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
}

void DeferredRenderer::EndGeometryPass() {
    if (!m_GBuffer) return;
    m_GBuffer->Unbind();
}

void DeferredRenderer::BeginHPWaterGBufferPass() {
    if (!m_HPWaterGBuffer) return;

    m_HPWaterGBuffer->Bind();
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
}

void DeferredRenderer::EndHPWaterGBufferPass() {
    if (!m_HPWaterGBuffer) return;
    m_HPWaterGBuffer->Unbind();
}

bool DeferredRenderer::BeginHPWaterCausticAtlas(uint32_t tileResolution) {
    if (!m_Initialized)
        return false;

    tileResolution = std::clamp(tileResolution, 128u, 2048u);
    CreateHPWaterCausticAtlasFBO(tileResolution);
    if (!m_HPWaterCausticAtlasFBO)
        return false;

    m_HPWaterCausticAtlasFBO->Bind();
    glViewport(0, 0,
               static_cast<GLsizei>(m_HPWaterCausticAtlasFBO->GetWidth()),
               static_cast<GLsizei>(m_HPWaterCausticAtlasFBO->GetHeight()));
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    m_HPWaterCausticAtlasValid = false;
    m_HPWaterCausticAtlasConsumed = false;
    return true;
}

void DeferredRenderer::BeginHPWaterCausticAtlasCascade(uint32_t cascadeIndex) {
    if (!m_HPWaterCausticAtlasFBO || m_HPWaterCausticAtlasTileResolution == 0)
        return;

    cascadeIndex = std::min(cascadeIndex, 3u);
    const GLint tile = static_cast<GLint>(m_HPWaterCausticAtlasTileResolution);
    const GLint x = static_cast<GLint>(cascadeIndex % 2u) * tile;
    const GLint y = static_cast<GLint>(cascadeIndex / 2u) * tile;
    glViewport(x, y, tile, tile);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, tile, tile);
}

void DeferredRenderer::EndHPWaterCausticAtlas(bool valid) {
    glDisable(GL_SCISSOR_TEST);
    if (m_HPWaterCausticAtlasFBO)
        m_HPWaterCausticAtlasFBO->Unbind();
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    m_HPWaterCausticAtlasValid = valid && m_HPWaterCausticAtlasFBO != nullptr;
}

void DeferredRenderer::BindGBufferTextures(int startUnit) {
    if (!m_GBuffer) return;

    static bool logged = false;
    for (int i = 0; i < m_GBuffer->GetColorAttachmentCount(); ++i) {
        GLuint texID = static_cast<GLuint>(m_GBuffer->GetColorAttachmentID(i));
        glActiveTexture(GL_TEXTURE0 + startUnit + i);
        glBindTexture(GL_TEXTURE_2D, texID);
        if (!logged)
            VE_ENGINE_INFO("[DEBUG] G-buffer RT{} texID={} bound to unit {}", i, texID, startUnit + i);
    }
    logged = true;
}

void DeferredRenderer::LightingPass() {
    if (!m_LightingShader || !m_LightingFBO || !m_GBuffer) return;
    m_HPWaterCompositeValid = false;
    m_HPWaterMaskValid = false;
    m_HPWaterVolumeValid = false;
    m_HPWaterVolumeFilteredValid = false;
    m_HPWaterVolumeUpsampledValid = false;
    m_HPWaterVolumeExplicitMotionVectorEnabled = false;
    m_HPWaterVolumeSceneMotionVectorEnabled = false;
    m_HPWaterVolumeMotionVectorHistoryEnabled = false;
    m_HPWaterVolumeExponentialIntegrationEnabled = false;
    m_HPWaterVolumeShadowSamplingEnabled = false;
    m_HPWaterVolumeShadowParamsEnabled = false;
    m_HPWaterVolumePunctualLightLoopEnabled = false;
    m_HPWaterVolumeAreaLightLoopEnabled = false;
    m_HPWaterVolumePointLightCount = 0;
    m_HPWaterVolumeSpotLightCount = 0;
    m_HPWaterVolumeAreaLightCount = 0;
    m_HPWaterVolumeShadowSoftness = 0.0f;
    m_HPWaterVolumeShadowMinFilterSize = 0.0f;
    m_HPWaterVolumeShadowBlockerSamples = 0;
    m_HPWaterVolumeShadowFilterSamples = 0;
    m_HPWaterVolumeSampleCount = 0;
    m_HPWaterVolumeTemporalBlendFactor = 0.0f;
    m_HPWaterVolumeSpatialFilterEnabled = false;
    m_HPWaterVolumeSpatialFilterIterations = 0;
    m_HPWaterVolumeMotionVectorsEnabled = false;
    m_HPWaterVolumeMotionVectorVelocityScale = 0.0f;
    m_HPWaterVolumeTemporalDepthRejectionEnabled = false;
    m_HPWaterVolumeTemporalDepthThreshold = 0.0f;
    m_HPWaterVolumeSpatialDepthAwareEnabled = false;
    m_HPWaterVolumeSpatialDepthSensitivity = 0.0f;
    m_HPWaterCausticValid = false;
    m_HPWaterCausticFilteredValid = false;
    m_HPWaterCausticFilterComputeParityEnabled = false;
    m_HPWaterCausticFilterLDSHaloEnabled = false;
    m_HPWaterDepthPyramidValid = false;
    m_HPWaterVolumeFilterIterations = 0;
    m_HPWaterCausticFilterIterations = 0;

    m_LightingFBO->Bind();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // No depth test for fullscreen quad
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

    m_LightingShader->Bind();

    // Bind G-buffer textures to units 0-3
    BindGBufferTextures(0);
    m_LightingShader->SetInt("u_GPositionMetallic", 0);
    m_LightingShader->SetInt("u_GNormalRoughness", 1);
    m_LightingShader->SetInt("u_GAlbedoAO", 2);
    m_LightingShader->SetInt("u_GEmissionFlags", 3);

    // Draw fullscreen triangle
    glBindVertexArray(m_QuadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    // Restore state
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);

    m_LightingFBO->Unbind();
}

bool DeferredRenderer::BuildHPWaterDepthPyramid() {
    if (!m_HPWaterDepthPyramidShader || !m_GBuffer || m_HPWaterDepthPyramidTexture == 0 ||
        m_HPWaterDepthPyramidFBO == 0 || m_HPWaterDepthPyramidMipCount == 0 || m_QuadVAO == 0) {
        m_HPWaterDepthPyramidValid = false;
        return false;
    }

    GLint previousViewport[4] = {};
    glGetIntegerv(GL_VIEWPORT, previousViewport);
    GLint previousDrawFBO = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFBO);
    GLint previousReadFBO = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFBO);
    GLboolean previousDepthMask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &previousDepthMask);
    const bool previousDepthTest = glIsEnabled(GL_DEPTH_TEST) == GL_TRUE;

    glBindFramebuffer(GL_FRAMEBUFFER, m_HPWaterDepthPyramidFBO);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glReadBuffer(GL_NONE);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

    m_HPWaterDepthPyramidShader->Bind();
    m_HPWaterDepthPyramidShader->SetInt("u_SourceDepth", 0);

    bool buildOk = true;
    glBindVertexArray(m_QuadVAO);
    for (uint32_t mip = 0; mip < m_HPWaterDepthPyramidMipCount; ++mip) {
        const uint32_t mipWidth = std::max(1u, m_Width >> mip);
        const uint32_t mipHeight = std::max(1u, m_Height >> mip);

        glFramebufferTexture2D(GL_FRAMEBUFFER,
                               GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D,
                               m_HPWaterDepthPyramidTexture,
                               static_cast<GLint>(mip));

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            VE_ENGINE_WARN("DeferredRenderer: HPWater depth pyramid FBO incomplete at mip {}", mip);
            buildOk = false;
            break;
        }

        glViewport(0, 0, static_cast<GLsizei>(mipWidth), static_cast<GLsizei>(mipHeight));
        glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        if (mip == 0) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_GBuffer->GetDepthAttachmentID()));
            m_HPWaterDepthPyramidShader->SetInt("u_FirstMip", 1);
            m_HPWaterDepthPyramidShader->SetInt("u_SourceMip", 0);
        } else {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, m_HPWaterDepthPyramidTexture);
            m_HPWaterDepthPyramidShader->SetInt("u_FirstMip", 0);
            m_HPWaterDepthPyramidShader->SetInt("u_SourceMip", static_cast<int>(mip - 1u));
        }

        glDrawArrays(GL_TRIANGLES, 0, 3);
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);
    }
    glBindVertexArray(0);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDrawFBO));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousReadFBO));
    glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
    if (previousDepthTest)
        glEnable(GL_DEPTH_TEST);
    else
        glDisable(GL_DEPTH_TEST);
    glDepthMask(previousDepthMask);

    m_HPWaterDepthPyramidValid = buildOk && m_HPWaterDepthPyramidMipCount > 0;
    return m_HPWaterDepthPyramidValid;
}

bool DeferredRenderer::MergeHPWaterDepthIntoSceneDepth() {
    if (!m_HPWaterDepthMergeShader || !m_GBuffer || !m_HPWaterGBuffer || m_QuadVAO == 0) {
        m_HPWaterDepthMergedToSceneDepth = false;
        m_HPWaterStencilMarkedInSceneDepth = false;
        m_HPWaterStencilRef = 0;
        return false;
    }

    const GLuint hpWaterDepth = static_cast<GLuint>(m_HPWaterGBuffer->GetDepthAttachmentID());
    if (hpWaterDepth == 0) {
        m_HPWaterDepthMergedToSceneDepth = false;
        m_HPWaterStencilMarkedInSceneDepth = false;
        m_HPWaterStencilRef = 0;
        return false;
    }

    GLint previousViewport[4] = {};
    glGetIntegerv(GL_VIEWPORT, previousViewport);
    GLint previousDrawFBO = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFBO);
    GLint previousReadFBO = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFBO);
    GLint previousDepthFunc = GL_LESS;
    glGetIntegerv(GL_DEPTH_FUNC, &previousDepthFunc);
    GLboolean previousDepthMask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &previousDepthMask);
    GLboolean previousColorMask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
    glGetBooleanv(GL_COLOR_WRITEMASK, previousColorMask);
    const bool previousDepthTest = glIsEnabled(GL_DEPTH_TEST) == GL_TRUE;
    const bool previousStencilTest = glIsEnabled(GL_STENCIL_TEST) == GL_TRUE;
    const bool previousCullFace = glIsEnabled(GL_CULL_FACE) == GL_TRUE;
    const bool previousBlend = glIsEnabled(GL_BLEND) == GL_TRUE;
    GLint previousStencilFunc = GL_ALWAYS;
    GLint previousStencilRef = 0;
    GLint previousStencilValueMask = 0xFF;
    GLint previousStencilWriteMask = 0xFF;
    GLint previousStencilFail = GL_KEEP;
    GLint previousStencilDepthFail = GL_KEEP;
    GLint previousStencilDepthPass = GL_KEEP;
    glGetIntegerv(GL_STENCIL_FUNC, &previousStencilFunc);
    glGetIntegerv(GL_STENCIL_REF, &previousStencilRef);
    glGetIntegerv(GL_STENCIL_VALUE_MASK, &previousStencilValueMask);
    glGetIntegerv(GL_STENCIL_WRITEMASK, &previousStencilWriteMask);
    glGetIntegerv(GL_STENCIL_FAIL, &previousStencilFail);
    glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &previousStencilDepthFail);
    glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &previousStencilDepthPass);

    m_GBuffer->Bind();
    glViewport(0, 0, static_cast<GLsizei>(m_Width), static_cast<GLsizei>(m_Height));
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(kHPWaterStencilRef);
    glStencilFunc(GL_ALWAYS, static_cast<GLint>(kHPWaterStencilRef), kHPWaterStencilRef);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    m_HPWaterDepthMergeShader->Bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, hpWaterDepth);
    m_HPWaterDepthMergeShader->SetInt("u_HPWaterDepth", 0);

    glBindVertexArray(m_QuadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glColorMask(previousColorMask[0],
                previousColorMask[1],
                previousColorMask[2],
                previousColorMask[3]);
    glDepthFunc(static_cast<GLenum>(previousDepthFunc));
    glDepthMask(previousDepthMask);
    glStencilMask(static_cast<GLuint>(previousStencilWriteMask));
    glStencilFunc(static_cast<GLenum>(previousStencilFunc),
                  previousStencilRef,
                  static_cast<GLuint>(previousStencilValueMask));
    glStencilOp(static_cast<GLenum>(previousStencilFail),
                static_cast<GLenum>(previousStencilDepthFail),
                static_cast<GLenum>(previousStencilDepthPass));
    if (previousDepthTest)
        glEnable(GL_DEPTH_TEST);
    else
        glDisable(GL_DEPTH_TEST);
    if (previousStencilTest)
        glEnable(GL_STENCIL_TEST);
    else
        glDisable(GL_STENCIL_TEST);
    if (previousCullFace)
        glEnable(GL_CULL_FACE);
    else
        glDisable(GL_CULL_FACE);
    if (previousBlend)
        glEnable(GL_BLEND);
    else
        glDisable(GL_BLEND);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDrawFBO));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousReadFBO));
    glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);

    m_HPWaterDepthMergedToSceneDepth = true;
    m_HPWaterStencilMarkedInSceneDepth = true;
    m_HPWaterStencilRef = kHPWaterStencilRef;
    return true;
}

bool DeferredRenderer::MergeHPWaterNormalIntoSceneGBuffer() {
    if (!m_HPWaterNormalMergeShader || !m_GBuffer || !m_HPWaterGBuffer || m_QuadVAO == 0) {
        m_HPWaterNormalMergedToSceneGBuffer = false;
        return false;
    }

    const GLuint hpWaterDepth = static_cast<GLuint>(m_HPWaterGBuffer->GetDepthAttachmentID());
    const GLuint hpWaterNormalRoughness = static_cast<GLuint>(m_HPWaterGBuffer->GetColorAttachmentID(0));
    if (hpWaterDepth == 0 || hpWaterNormalRoughness == 0) {
        m_HPWaterNormalMergedToSceneGBuffer = false;
        return false;
    }

    GLint previousViewport[4] = {};
    glGetIntegerv(GL_VIEWPORT, previousViewport);
    GLint previousDrawFBO = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFBO);
    GLint previousReadFBO = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFBO);
    GLint previousDepthFunc = GL_LESS;
    glGetIntegerv(GL_DEPTH_FUNC, &previousDepthFunc);
    GLboolean previousDepthMask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &previousDepthMask);
    GLboolean previousColorMask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
    glGetBooleanv(GL_COLOR_WRITEMASK, previousColorMask);
    const bool previousDepthTest = glIsEnabled(GL_DEPTH_TEST) == GL_TRUE;
    const bool previousCullFace = glIsEnabled(GL_CULL_FACE) == GL_TRUE;
    const bool previousBlend = glIsEnabled(GL_BLEND) == GL_TRUE;

    m_GBuffer->Bind();
    glViewport(0, 0, static_cast<GLsizei>(m_Width), static_cast<GLsizei>(m_Height));
    glDrawBuffer(GL_COLOR_ATTACHMENT1);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    m_HPWaterNormalMergeShader->Bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, hpWaterDepth);
    m_HPWaterNormalMergeShader->SetInt("u_HPWaterDepth", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, hpWaterNormalRoughness);
    m_HPWaterNormalMergeShader->SetInt("u_HPWaterNormalRoughness", 1);

    glBindVertexArray(m_QuadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    GLenum drawBuffers[4] = {
        GL_COLOR_ATTACHMENT0,
        GL_COLOR_ATTACHMENT1,
        GL_COLOR_ATTACHMENT2,
        GL_COLOR_ATTACHMENT3
    };
    glDrawBuffers(4, drawBuffers);
    glColorMask(previousColorMask[0],
                previousColorMask[1],
                previousColorMask[2],
                previousColorMask[3]);
    glDepthFunc(static_cast<GLenum>(previousDepthFunc));
    glDepthMask(previousDepthMask);
    if (previousDepthTest)
        glEnable(GL_DEPTH_TEST);
    else
        glDisable(GL_DEPTH_TEST);
    if (previousCullFace)
        glEnable(GL_CULL_FACE);
    else
        glDisable(GL_CULL_FACE);
    if (previousBlend)
        glEnable(GL_BLEND);
    else
        glDisable(GL_BLEND);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDrawFBO));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousReadFBO));
    glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);

    m_HPWaterNormalMergedToSceneGBuffer = true;
    return true;
}

bool DeferredRenderer::BuildHPWaterMask() {
    if (!m_HPWaterMaskShader || !m_HPWaterMaskFBO || !m_HPWaterGBuffer || m_QuadVAO == 0) {
        m_HPWaterMaskValid = false;
        return false;
    }

    m_HPWaterMaskFBO->Bind();
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

    m_HPWaterMaskShader->Bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_HPWaterGBuffer->GetDepthAttachmentID()));
    m_HPWaterMaskShader->SetInt("u_HPWaterDepth", 0);

    glBindVertexArray(m_QuadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);

    m_HPWaterMaskFBO->Unbind();
    m_HPWaterMaskValid = true;
    return true;
}

bool DeferredRenderer::CompositeHPWater(float nearClip,
                                        float farClip,
                                        float refractionStrength,
                                        float waterDispersionStrength,
                                        float maxRefractionCrossDistance,
                                        float refractionThicknessOffset,
                                        int refractionSampleCount,
                                       bool refractionJitter,
                                       uint32_t frameIndex,
                                       float environmentReflectionIntensity,
                                       float indirectLightStrength,
                                       float thinSSSStrength,
                                        float backlitTransmissionStrength,
                                        float forwardScatterStrength,
                                        float forwardScatterBlurDensity,
                                        float multiScatterScale,
                                        float phaseG,
                                        float specularFGDStrength,
                                        float ggxEnergyCompensation,
                                        const glm::vec3& cameraPosition,
                                        const glm::vec3& lightDir,
                                        const glm::vec3& lightColor,
                                        float lightIntensity,
                                        int pointLightCount,
                                        const std::array<glm::vec3, 8>& pointLightPositions,
                                        const std::array<glm::vec3, 8>& pointLightColors,
                                        const std::array<float, 8>& pointLightIntensities,
                                        const std::array<float, 8>& pointLightRanges,
                                        int spotLightCount,
                                        const std::array<glm::vec3, 4>& spotLightPositions,
                                        const std::array<glm::vec3, 4>& spotLightDirections,
                                        const std::array<glm::vec3, 4>& spotLightColors,
                                        const std::array<float, 4>& spotLightIntensities,
                                        const std::array<float, 4>& spotLightRanges,
                                        const std::array<float, 4>& spotLightInnerCos,
                                        const std::array<float, 4>& spotLightOuterCos,
                                        int areaLightCount,
                                        const std::array<glm::vec3, 4>& areaLightPositions,
                                        const std::array<glm::vec3, 4>& areaLightRights,
                                        const std::array<glm::vec3, 4>& areaLightUps,
                                        const std::array<glm::vec3, 4>& areaLightForwards,
                                        const std::array<glm::vec3, 4>& areaLightColors,
                                        const std::array<float, 4>& areaLightIntensities,
                                        const std::array<float, 4>& areaLightRanges,
                                        const std::array<float, 4>& areaLightWidths,
                                        const std::array<float, 4>& areaLightHeights,
                                        const glm::mat4& shadowCameraView,
                                        const std::array<glm::mat4, 4>& shadowLightVP,
                                        const std::array<float, 4>& shadowCascadeSplits,
                                        uint32_t shadowDepthTextureArray,
                                        uint32_t shadowDepthResolution,
                                        bool shadowsEnabled,
                                        float shadowDepthBias,
                                        float shadowNormalBias,
                                        int shadowPCFQuality,
                                        float shadowCascadeBlendWidth,
                                        const glm::vec3& indirectSkyColor,
                                        const glm::vec3& indirectGroundColor,
                                        const glm::vec3& indirectTint,
                                        bool indirectLightingEnabled,
                                        float indirectDiffuseIntensity,
                                        float skyReflectionIntensity,
                                        bool hpWaterSSREnabled,
                                        int hpWaterSSRMaxSteps,
                                        float hpWaterSSRStepSize,
                                        float hpWaterSSRThickness,
                                        float hpWaterSSRMaxDistance,
                                        uint32_t skyTexture,
                                        uint32_t reflectionProbeTexture,
                                        uint32_t reflectionProbeSecondaryTexture,
                                        bool hasReflectionProbe,
                                        float reflectionProbeIntensity,
                                        float reflectionProbeBlend,
                                        float reflectionProbeHierarchyWeight,
                                        const glm::vec3& reflectionProbeCenter,
                                        const glm::vec3& reflectionProbeBoxSize,
                                        const glm::vec3& reflectionProbeSecondaryCenter,
                                        const glm::vec3& reflectionProbeSecondaryBoxSize,
                                        const glm::mat4& viewProjection,
                                        const glm::mat4& inverseViewProjection) {
    if (!m_HPWaterCompositeShader || !m_HPWaterCompositeFBO || !m_LightingFBO ||
        !m_GBuffer || !m_HPWaterGBuffer || m_QuadVAO == 0) {
        m_HPWaterCompositeValid = false;
        m_HPWaterRefractionNDCMarchEnabled = false;
        m_HPWaterSurfaceShadowSamplingEnabled = false;
        m_HPWaterShadowCascadeDitherEnabled = false;
        return false;
    }

    m_HPWaterCompositeFBO->Bind();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

    m_HPWaterCompositeShader->Bind();

    m_HPWaterSceneColorMipCount = 1;
    m_HPWaterSceneColorMipValid = false;
    const GLuint sceneColorTexture = static_cast<GLuint>(m_LightingFBO->GetColorAttachmentID());
    if (sceneColorTexture != 0 && m_Width > 1 && m_Height > 1) {
        m_HPWaterSceneColorMipCount = CalculateMipCount(m_Width, m_Height);
        glBindTexture(GL_TEXTURE_2D, sceneColorTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,
            static_cast<GLint>(m_HPWaterSceneColorMipCount - 1u));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glGenerateMipmap(GL_TEXTURE_2D);
        m_HPWaterSceneColorMipValid = true;
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneColorTexture);
    m_HPWaterCompositeShader->SetInt("u_SceneColor", 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_GBuffer->GetDepthAttachmentID()));
    m_HPWaterCompositeShader->SetInt("u_SceneDepth", 1);

    BindHPWaterGBufferTextures(2);
    m_HPWaterCompositeShader->SetInt("u_HPWaterNormalRoughness", 2);
    m_HPWaterCompositeShader->SetInt("u_HPWaterScatterThickness", 3);
    m_HPWaterCompositeShader->SetInt("u_HPWaterAbsorptionFoam", 4);

    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_HPWaterGBuffer->GetDepthAttachmentID()));
    m_HPWaterCompositeShader->SetInt("u_HPWaterDepth", 5);

    auto volumeForComposite = m_HPWaterVolumeUpsampledValid && m_HPWaterVolumeUpsampledFBO
        ? m_HPWaterVolumeUpsampledFBO
        : (m_HPWaterVolumeFilteredValid && m_HPWaterVolumeFilteredFBO
            ? m_HPWaterVolumeFilteredFBO
            : m_HPWaterVolumeFBO);

    if (volumeForComposite) {
        glActiveTexture(GL_TEXTURE6);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(volumeForComposite->GetColorAttachmentID(0)));
        m_HPWaterCompositeShader->SetInt("u_HPWaterVolumeColor", 6);

        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(volumeForComposite->GetColorAttachmentID(1)));
        m_HPWaterCompositeShader->SetInt("u_HPWaterVolumeTransmittance", 7);

        glActiveTexture(GL_TEXTURE8);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(volumeForComposite->GetColorAttachmentID(2)));
        m_HPWaterCompositeShader->SetInt("u_HPWaterVolumeDepth", 8);
    }

    glActiveTexture(GL_TEXTURE9);
    glBindTexture(GL_TEXTURE_2D, m_HPWaterDepthPyramidValid
        ? m_HPWaterDepthPyramidTexture
        : static_cast<GLuint>(m_GBuffer->GetDepthAttachmentID()));
    m_HPWaterCompositeShader->SetInt("u_HPWaterDepthPyramid", 9);

    glActiveTexture(GL_TEXTURE10);
    glBindTexture(GL_TEXTURE_2D, m_HPWaterMaskValid && m_HPWaterMaskFBO
        ? static_cast<GLuint>(m_HPWaterMaskFBO->GetColorAttachmentID())
        : static_cast<GLuint>(m_HPWaterGBuffer->GetDepthAttachmentID()));
    m_HPWaterCompositeShader->SetInt("u_HPWaterMask", 10);

    glActiveTexture(GL_TEXTURE11);
    const auto causticForComposite = m_HPWaterCausticFilteredValid && m_HPWaterCausticFilteredFBO
        ? m_HPWaterCausticFilteredFBO
        : m_HPWaterCausticFBO;
    const bool causticCompositeValid = m_HPWaterCausticFilteredValid || m_HPWaterCausticValid;
    glBindTexture(GL_TEXTURE_2D, causticCompositeValid && causticForComposite
        ? static_cast<GLuint>(causticForComposite->GetColorAttachmentID())
        : 0);
    m_HPWaterCompositeShader->SetInt("u_HPWaterCaustic", 11);

    const bool skyTextureValid = skyTexture != 0;
    glActiveTexture(GL_TEXTURE12);
    glBindTexture(GL_TEXTURE_2D, skyTextureValid ? static_cast<GLuint>(skyTexture) : 0);
    m_HPWaterCompositeShader->SetInt("u_SkyTexture", 12);
    m_HPWaterCompositeShader->SetInt("u_HasSkyTexture", skyTextureValid ? 1 : 0);

    const bool reflectionProbeValid = hasReflectionProbe && reflectionProbeTexture != 0;
    const bool reflectionProbeSecondaryValid = reflectionProbeValid && reflectionProbeSecondaryTexture != 0;
    glActiveTexture(GL_TEXTURE13);
    glBindTexture(GL_TEXTURE_CUBE_MAP, reflectionProbeValid ? static_cast<GLuint>(reflectionProbeTexture) : 0);
    m_HPWaterCompositeShader->SetInt("u_ReflectionProbe", 13);

    glActiveTexture(GL_TEXTURE15);
    glBindTexture(GL_TEXTURE_CUBE_MAP, reflectionProbeSecondaryValid
        ? static_cast<GLuint>(reflectionProbeSecondaryTexture)
        : (reflectionProbeValid ? static_cast<GLuint>(reflectionProbeTexture) : 0));
    m_HPWaterCompositeShader->SetInt("u_ReflectionProbeSecondary", 15);

    const bool surfaceShadowSamplingValid = shadowsEnabled && shadowDepthTextureArray != 0 &&
        shadowDepthResolution > 0;
    const bool shadowCascadeDitherEnabled = surfaceShadowSamplingValid &&
        shadowCascadeBlendWidth > 0.0001f;
    glActiveTexture(GL_TEXTURE16);
    glBindTexture(GL_TEXTURE_2D_ARRAY, surfaceShadowSamplingValid
        ? static_cast<GLuint>(shadowDepthTextureArray)
        : 0);
    m_HPWaterCompositeShader->SetInt("u_ShadowMap", 16);

    m_HPWaterCompositeShader->SetInt("u_HasReflectionProbe", reflectionProbeValid ? 1 : 0);
    m_HPWaterCompositeShader->SetFloat("u_ReflectionProbeIntensity",
        reflectionProbeValid ? std::clamp(reflectionProbeIntensity, 0.0f, 4.0f) : 0.0f);
    m_HPWaterCompositeShader->SetFloat("u_ReflectionProbeBlend",
        reflectionProbeSecondaryValid ? std::clamp(reflectionProbeBlend, 0.0f, 1.0f) : 0.0f);
    m_HPWaterCompositeShader->SetFloat("u_ReflectionProbeHierarchyWeight",
        reflectionProbeValid ? std::clamp(reflectionProbeHierarchyWeight, 0.0f, 1.0f) : 0.0f);
    const glm::vec3 primaryBoxSize = glm::max(reflectionProbeBoxSize, glm::vec3(0.001f));
    const glm::vec3 secondaryBoxSize = glm::max(reflectionProbeSecondaryBoxSize, glm::vec3(0.001f));
    m_HPWaterCompositeShader->SetInt("u_ReflectionProbeBoxProjectionEnabled", reflectionProbeValid ? 1 : 0);
    m_HPWaterCompositeShader->SetVec3("u_ReflectionProbeCenter", reflectionProbeCenter);
    m_HPWaterCompositeShader->SetVec3("u_ReflectionProbeBoxMin", reflectionProbeCenter - primaryBoxSize * 0.5f);
    m_HPWaterCompositeShader->SetVec3("u_ReflectionProbeBoxMax", reflectionProbeCenter + primaryBoxSize * 0.5f);
    m_HPWaterCompositeShader->SetVec3("u_ReflectionProbeSecondaryCenter",
        reflectionProbeSecondaryValid ? reflectionProbeSecondaryCenter : reflectionProbeCenter);
    m_HPWaterCompositeShader->SetVec3("u_ReflectionProbeSecondaryBoxMin",
        (reflectionProbeSecondaryValid ? reflectionProbeSecondaryCenter : reflectionProbeCenter) -
            (reflectionProbeSecondaryValid ? secondaryBoxSize : primaryBoxSize) * 0.5f);
    m_HPWaterCompositeShader->SetVec3("u_ReflectionProbeSecondaryBoxMax",
        (reflectionProbeSecondaryValid ? reflectionProbeSecondaryCenter : reflectionProbeCenter) +
            (reflectionProbeSecondaryValid ? secondaryBoxSize : primaryBoxSize) * 0.5f);

    glActiveTexture(GL_TEXTURE14);
    glBindTexture(GL_TEXTURE_2D, m_HPWaterFGDLUTValid ? m_HPWaterFGDLUTTexture : 0);
    m_HPWaterCompositeShader->SetInt("u_PreintegratedFGDLUT", 14);
    m_HPWaterCompositeShader->SetInt("u_PreintegratedFGDLUTEnabled", m_HPWaterFGDLUTValid ? 1 : 0);

    m_HPWaterCompositeShader->SetFloat("u_NearClip", nearClip);
    m_HPWaterCompositeShader->SetFloat("u_FarClip", farClip);
    m_HPWaterCompositeShader->SetFloat("u_RefractionStrength", std::clamp(refractionStrength, 0.0f, 2.0f));
    m_HPWaterCompositeShader->SetFloat("u_WaterDispersionStrength",
        std::clamp(waterDispersionStrength, 0.0f, 2.0f));
    m_HPWaterCompositeShader->SetFloat("u_MaxRefractionCrossDistance",
        std::clamp(maxRefractionCrossDistance, 0.1f, 200.0f));
    m_HPWaterCompositeShader->SetFloat("u_RefractionThicknessOffset",
        std::clamp(refractionThicknessOffset, 0.01f, 8.0f));
    m_HPWaterCompositeShader->SetInt("u_RefractionSampleCount", std::clamp(refractionSampleCount, 4, 64));
    m_HPWaterCompositeShader->SetInt("u_RefractionJitterEnabled", refractionJitter ? 1 : 0);
    m_HPWaterCompositeShader->SetInt("u_FrameIndex", static_cast<int>(frameIndex & 0x7fffffffU));
    m_HPWaterCompositeShader->SetFloat("u_EnvironmentReflectionIntensity",
        std::clamp(environmentReflectionIntensity, 0.0f, 3.0f));
    m_HPWaterCompositeShader->SetFloat("u_IndirectLightStrength",
        std::clamp(indirectLightStrength, 0.0f, 4.0f));
    m_HPWaterCompositeShader->SetFloat("u_ThinSSSStrength",
        std::clamp(thinSSSStrength, 0.0f, 3.0f));
    m_HPWaterCompositeShader->SetFloat("u_BacklitTransmissionStrength",
        std::clamp(backlitTransmissionStrength, 0.0f, 3.0f));
    m_HPWaterCompositeShader->SetFloat("u_ForwardScatterStrength",
        std::clamp(forwardScatterStrength, 0.0f, 3.0f));
    m_HPWaterCompositeShader->SetFloat("u_ForwardScatterBlurDensity",
        std::clamp(forwardScatterBlurDensity, 0.0f, 4.0f));
    m_HPWaterCompositeShader->SetFloat("u_MultiScatterScale",
        std::clamp(multiScatterScale, 0.0f, 32.0f));
    m_HPWaterCompositeShader->SetFloat("u_PhaseG",
        std::clamp(phaseG, -0.95f, 0.95f));
    m_HPWaterCompositeShader->SetFloat("u_SpecularFGDStrength",
        std::clamp(specularFGDStrength, 0.0f, 1.0f));
    m_HPWaterCompositeShader->SetFloat("u_GGXEnergyCompensation",
        std::clamp(ggxEnergyCompensation, 0.0f, 2.0f));
    const glm::vec3 safeLightDir = glm::length(lightDir) > 0.0001f
        ? glm::normalize(lightDir)
        : glm::normalize(glm::vec3(-0.35f, 0.82f, 0.44f));
    m_HPWaterCompositeShader->SetVec3("u_ViewPos", cameraPosition);
    m_HPWaterCompositeShader->SetVec3("u_LightDir", safeLightDir);
    m_HPWaterCompositeShader->SetVec3("u_LightColor", lightColor);
    m_HPWaterCompositeShader->SetFloat("u_LightIntensity", std::max(lightIntensity, 0.0f));
    const int clampedPointLightCount = std::clamp(pointLightCount, 0, 8);
    m_HPWaterCompositeShader->SetInt("u_NumPointLights", clampedPointLightCount);
    for (int i = 0; i < clampedPointLightCount; ++i) {
        const std::string index = std::to_string(i);
        m_HPWaterCompositeShader->SetVec3("u_PointLightPositions[" + index + "]", pointLightPositions[i]);
        m_HPWaterCompositeShader->SetVec3("u_PointLightColors[" + index + "]", pointLightColors[i]);
        m_HPWaterCompositeShader->SetFloat("u_PointLightIntensities[" + index + "]",
            std::max(pointLightIntensities[i], 0.0f));
        m_HPWaterCompositeShader->SetFloat("u_PointLightRanges[" + index + "]",
            std::max(pointLightRanges[i], 0.001f));
    }
    const int clampedSpotLightCount = std::clamp(spotLightCount, 0, 4);
    m_HPWaterCompositeShader->SetInt("u_NumSpotLights", clampedSpotLightCount);
    for (int i = 0; i < clampedSpotLightCount; ++i) {
        const std::string index = std::to_string(i);
        m_HPWaterCompositeShader->SetVec3("u_SpotLightPositions[" + index + "]", spotLightPositions[i]);
        m_HPWaterCompositeShader->SetVec3("u_SpotLightDirections[" + index + "]", spotLightDirections[i]);
        m_HPWaterCompositeShader->SetVec3("u_SpotLightColors[" + index + "]", spotLightColors[i]);
        m_HPWaterCompositeShader->SetFloat("u_SpotLightIntensities[" + index + "]",
            std::max(spotLightIntensities[i], 0.0f));
        m_HPWaterCompositeShader->SetFloat("u_SpotLightRanges[" + index + "]",
            std::max(spotLightRanges[i], 0.001f));
        m_HPWaterCompositeShader->SetFloat("u_SpotLightInnerCos[" + index + "]", spotLightInnerCos[i]);
        m_HPWaterCompositeShader->SetFloat("u_SpotLightOuterCos[" + index + "]", spotLightOuterCos[i]);
    }
    const int clampedAreaLightCount = std::clamp(areaLightCount, 0, 4);
    m_HPWaterCompositeShader->SetInt("u_NumAreaLights", clampedAreaLightCount);
    for (int i = 0; i < clampedAreaLightCount; ++i) {
        const std::string index = std::to_string(i);
        m_HPWaterCompositeShader->SetVec3("u_AreaLightPositions[" + index + "]", areaLightPositions[i]);
        m_HPWaterCompositeShader->SetVec3("u_AreaLightRights[" + index + "]", areaLightRights[i]);
        m_HPWaterCompositeShader->SetVec3("u_AreaLightUps[" + index + "]", areaLightUps[i]);
        m_HPWaterCompositeShader->SetVec3("u_AreaLightForwards[" + index + "]", areaLightForwards[i]);
        m_HPWaterCompositeShader->SetVec3("u_AreaLightColors[" + index + "]", areaLightColors[i]);
        m_HPWaterCompositeShader->SetFloat("u_AreaLightIntensities[" + index + "]",
            std::max(areaLightIntensities[i], 0.0f));
        m_HPWaterCompositeShader->SetFloat("u_AreaLightRanges[" + index + "]",
            std::max(areaLightRanges[i], 0.001f));
        m_HPWaterCompositeShader->SetFloat("u_AreaLightWidths[" + index + "]",
            std::max(areaLightWidths[i], 0.001f));
        m_HPWaterCompositeShader->SetFloat("u_AreaLightHeights[" + index + "]",
            std::max(areaLightHeights[i], 0.001f));
    }
    m_HPWaterCompositeShader->SetInt("u_ShadowsEnabled", surfaceShadowSamplingValid ? 1 : 0);
    m_HPWaterCompositeShader->SetMat4("u_ShadowCameraView", shadowCameraView);
    m_HPWaterCompositeShader->SetFloat("u_ShadowDepthBias", shadowDepthBias);
    m_HPWaterCompositeShader->SetFloat("u_ShadowNormalBias", shadowNormalBias);
    m_HPWaterCompositeShader->SetInt("u_ShadowPCFQuality", std::clamp(shadowPCFQuality, 0, 2));
    m_HPWaterCompositeShader->SetFloat("u_ShadowCascadeBlendWidth",
        std::clamp(shadowCascadeBlendWidth, 0.0f, 1.0f));
    m_HPWaterCompositeShader->SetInt("u_ShadowCascadeDitherEnabled",
        shadowCascadeDitherEnabled ? 1 : 0);
    m_HPWaterCompositeShader->SetInt("u_ShadowFrameIndex", static_cast<int>(frameIndex & 0x7fffffffU));
    for (int i = 0; i < 4; ++i) {
        const std::string index = std::to_string(i);
        m_HPWaterCompositeShader->SetMat4("u_ShadowLightVP[" + index + "]",
            shadowLightVP[static_cast<size_t>(i)]);
        m_HPWaterCompositeShader->SetFloat("u_ShadowCascadeSplits[" + index + "]",
            shadowCascadeSplits[static_cast<size_t>(i)]);
    }
    m_HPWaterCompositeShader->SetVec3("u_IndirectSkyColor", indirectSkyColor);
    m_HPWaterCompositeShader->SetVec3("u_IndirectGroundColor", indirectGroundColor);
    m_HPWaterCompositeShader->SetVec3("u_IndirectTint", indirectTint);
    m_HPWaterCompositeShader->SetInt("u_IndirectLightingEnabled", indirectLightingEnabled ? 1 : 0);
    m_HPWaterCompositeShader->SetFloat("u_IndirectDiffuseIntensity",
        std::clamp(indirectDiffuseIntensity, 0.0f, 4.0f));
    m_HPWaterCompositeShader->SetFloat("u_SkyReflectionIntensity",
        std::clamp(skyReflectionIntensity, 0.0f, 4.0f));
    const bool hpWaterSSRActive = hpWaterSSREnabled && hpWaterSSRMaxSteps > 0 && hpWaterSSRMaxDistance > 0.0001f;
    m_HPWaterCompositeShader->SetInt("u_HPWaterSSREnabled", hpWaterSSRActive ? 1 : 0);
    m_HPWaterCompositeShader->SetInt("u_HPWaterSSRMaxSteps", std::clamp(hpWaterSSRMaxSteps, 1, 128));
    m_HPWaterCompositeShader->SetFloat("u_HPWaterSSRStepSize",
        std::clamp(hpWaterSSRStepSize, 0.005f, 5.0f));
    m_HPWaterCompositeShader->SetFloat("u_HPWaterSSRThickness",
        std::clamp(hpWaterSSRThickness, 0.005f, 5.0f));
    m_HPWaterCompositeShader->SetFloat("u_HPWaterSSRMaxDistance",
        std::clamp(hpWaterSSRMaxDistance, 0.1f, 500.0f));
    m_HPWaterCompositeShader->SetFloat("u_HPWaterSSRStrength", hpWaterSSRActive ? 1.0f : 0.0f);
    m_HPWaterCompositeShader->SetMat4("u_ViewProjection", viewProjection);
    m_HPWaterCompositeShader->SetMat4("u_InverseViewProjection", inverseViewProjection);
    m_HPWaterCompositeShader->SetInt("u_HPWaterVolumeEnabled",
        (m_HPWaterVolumeValid || m_HPWaterVolumeFilteredValid || m_HPWaterVolumeUpsampledValid) ? 1 : 0);
    m_HPWaterCompositeShader->SetInt("u_HPWaterDepthPyramidEnabled", m_HPWaterDepthPyramidValid ? 1 : 0);
    m_HPWaterCompositeShader->SetInt("u_HPWaterDepthPyramidMipCount",
        static_cast<int>(m_HPWaterDepthPyramidMipCount));
    m_HPWaterCompositeShader->SetInt("u_SceneColorMipEnabled", m_HPWaterSceneColorMipValid ? 1 : 0);
    m_HPWaterCompositeShader->SetInt("u_SceneColorMipCount",
        static_cast<int>(m_HPWaterSceneColorMipCount));
    m_HPWaterCompositeShader->SetInt("u_HPWaterMaskEnabled", m_HPWaterMaskValid ? 1 : 0);
    m_HPWaterCompositeShader->SetInt("u_HPWaterCausticEnabled", causticCompositeValid ? 1 : 0);

    glBindVertexArray(m_QuadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);

    m_HPWaterCompositeFBO->Unbind();
    m_HPWaterCompositeValid = true;
    m_HPWaterSurfaceShadowSamplingEnabled = surfaceShadowSamplingValid;
    m_HPWaterShadowCascadeDitherEnabled = shadowCascadeDitherEnabled;
    m_HPWaterRefractionNDCMarchEnabled =
        m_HPWaterDepthPyramidValid &&
        refractionSampleCount > 0 &&
        refractionStrength > 0.0001f &&
        maxRefractionCrossDistance > 0.0001f;
    return true;
}

bool DeferredRenderer::AccumulateHPWaterVolume(float nearClip,
                                               float farClip,
                                               const glm::vec3& lightDir,
                                               const glm::vec3& lightColor,
                                               float lightIntensity,
                                               int pointLightCount,
                                               const std::array<glm::vec3, 8>& pointLightPositions,
                                               const std::array<glm::vec3, 8>& pointLightColors,
                                               const std::array<float, 8>& pointLightIntensities,
                                               const std::array<float, 8>& pointLightRanges,
                                               int spotLightCount,
                                               const std::array<glm::vec3, 4>& spotLightPositions,
                                               const std::array<glm::vec3, 4>& spotLightDirections,
                                               const std::array<glm::vec3, 4>& spotLightColors,
                                               const std::array<float, 4>& spotLightIntensities,
                                               const std::array<float, 4>& spotLightRanges,
                                               const std::array<float, 4>& spotLightInnerCos,
                                               const std::array<float, 4>& spotLightOuterCos,
                                               int areaLightCount,
                                               const std::array<glm::vec3, 4>& areaLightPositions,
                                               const std::array<glm::vec3, 4>& areaLightRights,
                                               const std::array<glm::vec3, 4>& areaLightUps,
                                               const std::array<glm::vec3, 4>& areaLightForwards,
                                               const std::array<glm::vec3, 4>& areaLightColors,
                                               const std::array<float, 4>& areaLightIntensities,
                                               const std::array<float, 4>& areaLightRanges,
                                               const std::array<float, 4>& areaLightWidths,
                                               const std::array<float, 4>& areaLightHeights,
                                               const glm::vec3& cameraPosition,
                                               const glm::mat4& inverseViewProjection,
                                               const glm::mat4& shadowCameraView,
                                               const std::array<glm::mat4, 4>& shadowLightVP,
                                               const std::array<float, 4>& shadowCascadeSplits,
                                               uint32_t shadowDepthTextureArray,
                                               uint32_t shadowDepthResolution,
                                               bool shadowsEnabled,
                                               float shadowDepthBias,
                                               float shadowNormalBias,
                                               int shadowPCFQuality,
                                               float shadowCascadeBlendWidth,
                                               float macroScatterStrength,
                                               float volumeShadowSoftness,
                                               float volumeShadowMinFilterSize,
                                               int volumeShadowBlockerSamples,
                                               int volumeShadowFilterSamples,
                                               float causticVolumeStrength,
                                               uint32_t frameIndex) {
    if (!m_HPWaterVolumeShader || !m_HPWaterVolumeFBO || !m_HPWaterCompositeFBO ||
        !m_GBuffer || !m_HPWaterGBuffer || m_QuadVAO == 0) {
        m_HPWaterVolumeValid = false;
        m_HPWaterVolumeExponentialIntegrationEnabled = false;
        m_HPWaterVolumeShadowSamplingEnabled = false;
        m_HPWaterShadowCascadeDitherEnabled = false;
        m_HPWaterVolumeShadowParamsEnabled = false;
        m_HPWaterVolumePunctualLightLoopEnabled = false;
        m_HPWaterVolumeAreaLightLoopEnabled = false;
        m_HPWaterVolumePointLightCount = 0;
        m_HPWaterVolumeSpotLightCount = 0;
        m_HPWaterVolumeAreaLightCount = 0;
        m_HPWaterVolumeShadowSoftness = 0.0f;
        m_HPWaterVolumeShadowMinFilterSize = 0.0f;
        m_HPWaterVolumeShadowBlockerSamples = 0;
        m_HPWaterVolumeShadowFilterSamples = 0;
        m_HPWaterVolumeSampleCount = 0;
        return false;
    }

    m_HPWaterVolumeFBO->Bind();
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

    m_HPWaterVolumeShader->Bind();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_LightingFBO->GetColorAttachmentID()));
    m_HPWaterVolumeShader->SetInt("u_SceneColor", 0);

    BindHPWaterGBufferTextures(1);
    m_HPWaterVolumeShader->SetInt("u_HPWaterNormalRoughness", 1);
    m_HPWaterVolumeShader->SetInt("u_HPWaterScatterThickness", 2);
    m_HPWaterVolumeShader->SetInt("u_HPWaterAbsorptionFoam", 3);

    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_HPWaterGBuffer->GetDepthAttachmentID()));
    m_HPWaterVolumeShader->SetInt("u_HPWaterDepth", 4);

    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_HPWaterCompositeFBO->GetColorAttachmentID(1)));
    m_HPWaterVolumeShader->SetInt("u_HPWaterRefractionWorldData", 5);

    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_HPWaterCompositeFBO->GetColorAttachmentID(2)));
    m_HPWaterVolumeShader->SetInt("u_HPWaterRefractionMeta", 6);

    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D, m_HPWaterMaskValid && m_HPWaterMaskFBO
        ? static_cast<GLuint>(m_HPWaterMaskFBO->GetColorAttachmentID())
        : static_cast<GLuint>(m_HPWaterGBuffer->GetDepthAttachmentID()));
    m_HPWaterVolumeShader->SetInt("u_HPWaterMask", 7);

    const auto causticForVolume = m_HPWaterCausticFilteredValid && m_HPWaterCausticFilteredFBO
        ? m_HPWaterCausticFilteredFBO
        : m_HPWaterCausticFBO;
    const bool causticVolumeValid = (m_HPWaterCausticFilteredValid || m_HPWaterCausticValid) &&
        causticForVolume && causticVolumeStrength > 0.0001f;
    glActiveTexture(GL_TEXTURE8);
    glBindTexture(GL_TEXTURE_2D, causticVolumeValid
        ? static_cast<GLuint>(causticForVolume->GetColorAttachmentID())
        : 0);
    m_HPWaterVolumeShader->SetInt("u_HPWaterCaustic", 8);

    const bool shadowSamplingValid = shadowsEnabled && shadowDepthTextureArray != 0 &&
        shadowDepthResolution > 0;
    const bool shadowCascadeDitherEnabled = shadowSamplingValid &&
        shadowCascadeBlendWidth > 0.0001f;
    const float clampedVolumeShadowSoftness = std::clamp(volumeShadowSoftness, 0.0f, 10.0f);
    const float clampedVolumeShadowMinFilterSize = std::clamp(volumeShadowMinFilterSize, 0.0f, 8.0f);
    const int clampedVolumeShadowBlockerSamples = std::clamp(volumeShadowBlockerSamples, 0, 16);
    const int clampedVolumeShadowFilterSamples = std::clamp(volumeShadowFilterSamples, 1, 16);
    const bool volumeShadowParamsEnabled = shadowSamplingValid &&
        (clampedVolumeShadowSoftness > 0.0001f || clampedVolumeShadowMinFilterSize > 0.0001f);
    glActiveTexture(GL_TEXTURE9);
    glBindTexture(GL_TEXTURE_2D_ARRAY,
        shadowSamplingValid ? static_cast<GLuint>(shadowDepthTextureArray) : 0);
    m_HPWaterVolumeShader->SetInt("u_ShadowMap", 9);

    m_HPWaterVolumeShader->SetFloat("u_NearClip", nearClip);
    m_HPWaterVolumeShader->SetFloat("u_FarClip", farClip);
    m_HPWaterVolumeShader->SetVec3("u_LightDir", lightDir);
    m_HPWaterVolumeShader->SetVec3("u_LightColor", lightColor);
    m_HPWaterVolumeShader->SetFloat("u_LightIntensity", lightIntensity);
    const int clampedPointLightCount = std::clamp(pointLightCount, 0, 8);
    m_HPWaterVolumeShader->SetInt("u_NumPointLights", clampedPointLightCount);
    for (int i = 0; i < clampedPointLightCount; ++i) {
        const std::string index = std::to_string(i);
        m_HPWaterVolumeShader->SetVec3("u_PointLightPositions[" + index + "]",
                                       pointLightPositions[i]);
        m_HPWaterVolumeShader->SetVec3("u_PointLightColors[" + index + "]",
                                       pointLightColors[i]);
        m_HPWaterVolumeShader->SetFloat("u_PointLightIntensities[" + index + "]",
                                        std::max(pointLightIntensities[i], 0.0f));
        m_HPWaterVolumeShader->SetFloat("u_PointLightRanges[" + index + "]",
                                        std::max(pointLightRanges[i], 0.001f));
    }
    const int clampedSpotLightCount = std::clamp(spotLightCount, 0, 4);
    m_HPWaterVolumeShader->SetInt("u_NumSpotLights", clampedSpotLightCount);
    for (int i = 0; i < clampedSpotLightCount; ++i) {
        const std::string index = std::to_string(i);
        m_HPWaterVolumeShader->SetVec3("u_SpotLightPositions[" + index + "]",
                                       spotLightPositions[i]);
        m_HPWaterVolumeShader->SetVec3("u_SpotLightDirections[" + index + "]",
                                       spotLightDirections[i]);
        m_HPWaterVolumeShader->SetVec3("u_SpotLightColors[" + index + "]",
                                       spotLightColors[i]);
        m_HPWaterVolumeShader->SetFloat("u_SpotLightIntensities[" + index + "]",
                                        std::max(spotLightIntensities[i], 0.0f));
        m_HPWaterVolumeShader->SetFloat("u_SpotLightRanges[" + index + "]",
                                        std::max(spotLightRanges[i], 0.001f));
        m_HPWaterVolumeShader->SetFloat("u_SpotLightInnerCos[" + index + "]",
                                        spotLightInnerCos[i]);
        m_HPWaterVolumeShader->SetFloat("u_SpotLightOuterCos[" + index + "]",
                                        spotLightOuterCos[i]);
    }
    const int clampedAreaLightCount = std::clamp(areaLightCount, 0, 4);
    m_HPWaterVolumeShader->SetInt("u_NumAreaLights", clampedAreaLightCount);
    for (int i = 0; i < clampedAreaLightCount; ++i) {
        const std::string index = std::to_string(i);
        m_HPWaterVolumeShader->SetVec3("u_AreaLightPositions[" + index + "]",
                                       areaLightPositions[i]);
        m_HPWaterVolumeShader->SetVec3("u_AreaLightRights[" + index + "]",
                                       areaLightRights[i]);
        m_HPWaterVolumeShader->SetVec3("u_AreaLightUps[" + index + "]",
                                       areaLightUps[i]);
        m_HPWaterVolumeShader->SetVec3("u_AreaLightForwards[" + index + "]",
                                       areaLightForwards[i]);
        m_HPWaterVolumeShader->SetVec3("u_AreaLightColors[" + index + "]",
                                       areaLightColors[i]);
        m_HPWaterVolumeShader->SetFloat("u_AreaLightIntensities[" + index + "]",
                                        std::max(areaLightIntensities[i], 0.0f));
        m_HPWaterVolumeShader->SetFloat("u_AreaLightRanges[" + index + "]",
                                        std::max(areaLightRanges[i], 0.001f));
        m_HPWaterVolumeShader->SetFloat("u_AreaLightWidths[" + index + "]",
                                        std::max(areaLightWidths[i], 0.001f));
        m_HPWaterVolumeShader->SetFloat("u_AreaLightHeights[" + index + "]",
                                        std::max(areaLightHeights[i], 0.001f));
    }
    m_HPWaterVolumeShader->SetVec3("u_CameraPosition", cameraPosition);
    m_HPWaterVolumeShader->SetMat4("u_InverseViewProjection", inverseViewProjection);
    m_HPWaterVolumeShader->SetFloat("u_MacroScatterStrength",
        std::clamp(macroScatterStrength, 0.0f, 4.0f));
    constexpr uint32_t hpWaterVolumeSampleCount = 16;
    m_HPWaterVolumeShader->SetInt("u_VolumeSampleCount",
        static_cast<int>(hpWaterVolumeSampleCount));
    m_HPWaterVolumeShader->SetInt("u_FrameIndex", static_cast<int>(frameIndex));
    m_HPWaterVolumeShader->SetInt("u_HPWaterMaskEnabled", m_HPWaterMaskValid ? 1 : 0);
    m_HPWaterVolumeShader->SetInt("u_HPWaterCausticEnabled", causticVolumeValid ? 1 : 0);
    m_HPWaterVolumeShader->SetFloat("u_CausticVolumeStrength",
        std::clamp(causticVolumeStrength, 0.0f, 4.0f));
    m_HPWaterVolumeShader->SetInt("u_ShadowsEnabled", shadowSamplingValid ? 1 : 0);
    m_HPWaterVolumeShader->SetMat4("u_ShadowCameraView", shadowCameraView);
    m_HPWaterVolumeShader->SetFloat("u_ShadowDepthBias", shadowDepthBias);
    m_HPWaterVolumeShader->SetFloat("u_ShadowNormalBias", shadowNormalBias);
    m_HPWaterVolumeShader->SetInt("u_ShadowPCFQuality", std::clamp(shadowPCFQuality, 0, 2));
    m_HPWaterVolumeShader->SetFloat("u_ShadowCascadeBlendWidth",
        std::clamp(shadowCascadeBlendWidth, 0.0f, 1.0f));
    m_HPWaterVolumeShader->SetInt("u_ShadowCascadeDitherEnabled",
        shadowCascadeDitherEnabled ? 1 : 0);
    m_HPWaterVolumeShader->SetInt("u_ShadowFrameIndex", static_cast<int>(frameIndex & 0x7fffffffU));
    m_HPWaterVolumeShader->SetInt("u_HPWaterVolumeShadowParamsEnabled",
        volumeShadowParamsEnabled ? 1 : 0);
    m_HPWaterVolumeShader->SetFloat("u_HPWaterVolumeShadowSoftness",
        clampedVolumeShadowSoftness);
    m_HPWaterVolumeShader->SetFloat("u_HPWaterVolumeShadowMinFilterSize",
        clampedVolumeShadowMinFilterSize);
    m_HPWaterVolumeShader->SetInt("u_HPWaterVolumeShadowBlockerSamples",
        clampedVolumeShadowBlockerSamples);
    m_HPWaterVolumeShader->SetInt("u_HPWaterVolumeShadowFilterSamples",
        clampedVolumeShadowFilterSamples);
    for (int i = 0; i < 4; ++i) {
        m_HPWaterVolumeShader->SetMat4("u_ShadowLightVP[" + std::to_string(i) + "]",
                                       shadowLightVP[static_cast<size_t>(i)]);
        m_HPWaterVolumeShader->SetFloat("u_ShadowCascadeSplits[" + std::to_string(i) + "]",
                                        shadowCascadeSplits[static_cast<size_t>(i)]);
    }

    glBindVertexArray(m_QuadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);

    m_HPWaterVolumeFBO->Unbind();
    m_HPWaterVolumeValid = true;
    m_HPWaterVolumeTemporalValid = false;
    m_HPWaterVolumeFilteredValid = false;
    m_HPWaterVolumeUpsampledValid = false;
    m_HPWaterVolumeFilterIterations = 0;
    m_HPWaterVolumeExponentialIntegrationEnabled = true;
    m_HPWaterVolumeShadowSamplingEnabled = shadowSamplingValid;
    m_HPWaterShadowCascadeDitherEnabled =
        m_HPWaterShadowCascadeDitherEnabled || shadowCascadeDitherEnabled;
    m_HPWaterVolumeShadowParamsEnabled = volumeShadowParamsEnabled;
    m_HPWaterVolumeShadowSoftness = clampedVolumeShadowSoftness;
    m_HPWaterVolumeShadowMinFilterSize = clampedVolumeShadowMinFilterSize;
    m_HPWaterVolumeShadowBlockerSamples = static_cast<uint32_t>(clampedVolumeShadowBlockerSamples);
    m_HPWaterVolumeShadowFilterSamples = static_cast<uint32_t>(clampedVolumeShadowFilterSamples);
    m_HPWaterVolumeSampleCount = hpWaterVolumeSampleCount;
    m_HPWaterVolumePunctualLightLoopEnabled = (clampedPointLightCount + clampedSpotLightCount) > 0;
    m_HPWaterVolumeAreaLightLoopEnabled = clampedAreaLightCount > 0;
    m_HPWaterVolumePointLightCount = static_cast<uint32_t>(clampedPointLightCount);
    m_HPWaterVolumeSpotLightCount = static_cast<uint32_t>(clampedSpotLightCount);
    m_HPWaterVolumeAreaLightCount = static_cast<uint32_t>(clampedAreaLightCount);
    return true;
}

bool DeferredRenderer::BuildHPWaterVolumeMotionVectors(const glm::mat4& currentViewProjection,
                                                       const glm::mat4& previousViewProjection) {
    if (!m_HPWaterVolumeMotionVectorShader || !m_HPWaterVolumeMotionVectorFBO ||
        !m_HPWaterVolumeFBO || !m_HPWaterCompositeFBO || !m_HPWaterVolumeValid || m_QuadVAO == 0) {
        m_HPWaterVolumeExplicitMotionVectorEnabled = false;
        m_HPWaterVolumeSceneMotionVectorEnabled = false;
        m_HPWaterVolumeMotionVectorHistoryEnabled = false;
        return false;
    }

    const uint32_t scenePositionTexture = m_GBuffer ? m_GBuffer->GetColorAttachmentID(0) : 0;
    const bool sceneMotionVectorInputValid = scenePositionTexture != 0;

    m_HPWaterVolumeMotionVectorFBO->Bind();
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

    m_HPWaterVolumeMotionVectorShader->Bind();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_HPWaterVolumeFBO->GetColorAttachmentID(2)));
    m_HPWaterVolumeMotionVectorShader->SetInt("u_CurrentDepth", 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_HPWaterCompositeFBO->GetColorAttachmentID(1)));
    m_HPWaterVolumeMotionVectorShader->SetInt("u_HPWaterRefractionWorldData", 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(scenePositionTexture));
    m_HPWaterVolumeMotionVectorShader->SetInt("u_ScenePositionMetallic", 2);

    m_HPWaterVolumeMotionVectorShader->SetMat4("u_CurrentViewProjection", currentViewProjection);
    m_HPWaterVolumeMotionVectorShader->SetMat4("u_PreviousViewProjection", previousViewProjection);
    m_HPWaterVolumeMotionVectorShader->SetInt("u_SceneMotionVectorEnabled",
        sceneMotionVectorInputValid ? 1 : 0);

    glBindVertexArray(m_QuadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);

    m_HPWaterVolumeMotionVectorFBO->Unbind();
    m_HPWaterVolumeExplicitMotionVectorEnabled = true;
    m_HPWaterVolumeSceneMotionVectorEnabled = sceneMotionVectorInputValid;
    return true;
}

bool DeferredRenderer::TemporalFilterHPWaterVolume(const glm::mat4& currentViewProjection,
                                                   const glm::mat4& previousViewProjection,
                                                   float temporalBlendFactor,
                                                   bool motionVectorsEnabled,
                                                   float motionVectorVelocityScale,
                                                   bool temporalDepthRejectionEnabled,
                                                   float temporalDepthThreshold) {
    if (!m_HPWaterVolumeTemporalShader || !m_HPWaterVolumeFBO || !m_HPWaterVolumeTemporalFBO ||
        !m_HPWaterVolumeHistoryFBO || !m_HPWaterCompositeFBO || !m_HPWaterVolumeValid || m_QuadVAO == 0) {
        m_HPWaterVolumeTemporalValid = false;
        m_HPWaterVolumeTemporalNeighborhoodClampEnabled = false;
        m_HPWaterVolumeTemporalMotionReprojectionEnabled = false;
        m_HPWaterVolumeExplicitMotionVectorEnabled = false;
        m_HPWaterVolumeSceneMotionVectorEnabled = false;
        m_HPWaterVolumeMotionVectorHistoryEnabled = false;
        m_HPWaterVolumeTemporalNeighborhoodClampStrength = 0.0f;
        m_HPWaterVolumeTemporalBlendFactor = 0.0f;
        m_HPWaterVolumeMotionVectorsEnabled = false;
        m_HPWaterVolumeMotionVectorVelocityScale = 0.0f;
        m_HPWaterVolumeTemporalDepthRejectionEnabled = false;
        m_HPWaterVolumeTemporalDepthThreshold = 0.0f;
        return false;
    }

    const float clampedHistoryBlend = std::clamp(temporalBlendFactor, 0.0f, 0.98f);
    const float clampedVelocityScale = std::clamp(motionVectorVelocityScale, 0.0f, 10.0f);
    const float clampedDepthThreshold = std::clamp(temporalDepthThreshold, 0.0001f, 10.0f);
    const bool motionVectorValid =
        motionVectorsEnabled && BuildHPWaterVolumeMotionVectors(currentViewProjection, previousViewProjection);

    m_HPWaterVolumeTemporalFBO->Bind();
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

    m_HPWaterVolumeTemporalShader->Bind();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_HPWaterVolumeFBO->GetColorAttachmentID(0)));
    m_HPWaterVolumeTemporalShader->SetInt("u_CurrentColor", 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_HPWaterVolumeFBO->GetColorAttachmentID(1)));
    m_HPWaterVolumeTemporalShader->SetInt("u_CurrentTransmittance", 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_HPWaterVolumeFBO->GetColorAttachmentID(2)));
    m_HPWaterVolumeTemporalShader->SetInt("u_CurrentDepth", 2);

    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_HPWaterVolumeHistoryFBO->GetColorAttachmentID(0)));
    m_HPWaterVolumeTemporalShader->SetInt("u_HistoryColor", 3);

    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_HPWaterVolumeHistoryFBO->GetColorAttachmentID(1)));
    m_HPWaterVolumeTemporalShader->SetInt("u_HistoryTransmittance", 4);

    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_HPWaterVolumeHistoryFBO->GetColorAttachmentID(2)));
    m_HPWaterVolumeTemporalShader->SetInt("u_HistoryDepth", 5);

    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_HPWaterCompositeFBO->GetColorAttachmentID(1)));
    m_HPWaterVolumeTemporalShader->SetInt("u_HPWaterRefractionWorldData", 6);

    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D,
        motionVectorValid && m_HPWaterVolumeMotionVectorFBO
            ? static_cast<GLuint>(m_HPWaterVolumeMotionVectorFBO->GetColorAttachmentID())
            : 0);
    m_HPWaterVolumeTemporalShader->SetInt("u_VolumeMotionVector", 7);

    m_HPWaterVolumeTemporalShader->SetMat4("u_CurrentViewProjection", currentViewProjection);
    m_HPWaterVolumeTemporalShader->SetMat4("u_PreviousViewProjection", previousViewProjection);
    m_HPWaterVolumeTemporalShader->SetInt("u_HistoryValid", m_HPWaterVolumeHistoryValid ? 1 : 0);
    m_HPWaterVolumeTemporalShader->SetInt("u_MotionVectorValid", motionVectorValid ? 1 : 0);
    m_HPWaterVolumeTemporalShader->SetInt("u_DepthRejectionEnabled", temporalDepthRejectionEnabled ? 1 : 0);
    m_HPWaterVolumeTemporalShader->SetFloat("u_HistoryBlend", clampedHistoryBlend);
    m_HPWaterVolumeTemporalShader->SetFloat("u_DepthRejectionThreshold", clampedDepthThreshold);
    m_HPWaterVolumeTemporalShader->SetFloat("u_MotionVectorVelocityScale", clampedVelocityScale);
    m_HPWaterVolumeTemporalShader->SetFloat("u_NeighborhoodClampStrength", 1.0f);

    glBindVertexArray(m_QuadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);

    m_HPWaterVolumeTemporalFBO->Unbind();
    m_HPWaterVolumeTemporalValid = true;
    m_HPWaterVolumeTemporalNeighborhoodClampEnabled = true;
    m_HPWaterVolumeTemporalMotionReprojectionEnabled = true;
    m_HPWaterVolumeExplicitMotionVectorEnabled = motionVectorValid;
    m_HPWaterVolumeSceneMotionVectorEnabled = motionVectorValid && m_HPWaterVolumeSceneMotionVectorEnabled;
    m_HPWaterVolumeMotionVectorHistoryEnabled = motionVectorValid;
    m_HPWaterVolumeTemporalNeighborhoodClampStrength = 1.0f;
    m_HPWaterVolumeTemporalBlendFactor = clampedHistoryBlend;
    m_HPWaterVolumeMotionVectorsEnabled = motionVectorsEnabled;
    m_HPWaterVolumeMotionVectorVelocityScale = clampedVelocityScale;
    m_HPWaterVolumeTemporalDepthRejectionEnabled = temporalDepthRejectionEnabled;
    m_HPWaterVolumeTemporalDepthThreshold = clampedDepthThreshold;
    m_HPWaterVolumeFilteredValid = false;
    m_HPWaterVolumeUpsampledValid = false;
    m_HPWaterVolumeFilterIterations = 0;
    return true;
}

void DeferredRenderer::CommitHPWaterVolumeHistory() {
    if (!m_HPWaterVolumeTemporalValid || !m_HPWaterVolumeTemporalFBO || !m_HPWaterVolumeHistoryFBO)
        return;

    std::swap(m_HPWaterVolumeHistoryFBO, m_HPWaterVolumeTemporalFBO);
    m_HPWaterVolumeHistoryValid = true;
    m_HPWaterVolumeTemporalValid = false;
}

void DeferredRenderer::InvalidateHPWaterVolumeHistory() {
    m_HPWaterVolumeTemporalValid = false;
    m_HPWaterVolumeHistoryValid = false;
    m_HPWaterVolumeUpsampledValid = false;
    m_HPWaterVolumeTemporalNeighborhoodClampEnabled = false;
    m_HPWaterVolumeTemporalMotionReprojectionEnabled = false;
    m_HPWaterVolumeExplicitMotionVectorEnabled = false;
    m_HPWaterVolumeSceneMotionVectorEnabled = false;
    m_HPWaterVolumeMotionVectorHistoryEnabled = false;
    m_HPWaterVolumeTemporalNeighborhoodClampStrength = 0.0f;
    m_HPWaterVolumeTemporalBlendFactor = 0.0f;
    m_HPWaterVolumeMotionVectorsEnabled = false;
    m_HPWaterVolumeMotionVectorVelocityScale = 0.0f;
    m_HPWaterVolumeTemporalDepthRejectionEnabled = false;
    m_HPWaterVolumeTemporalDepthThreshold = 0.0f;
}

bool DeferredRenderer::RunHPWaterVolumeFilterPass(const std::shared_ptr<Framebuffer>& inputFBO,
                                                  const std::shared_ptr<Framebuffer>& outputFBO,
                                                  float stride,
                                                  bool spatialDepthAwareEnabled,
                                                  float spatialDepthSensitivity) {
    if (!m_HPWaterVolumeFilterShader || !inputFBO || !outputFBO || m_QuadVAO == 0)
        return false;

    outputFBO->Bind();
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

    m_HPWaterVolumeFilterShader->Bind();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(inputFBO->GetColorAttachmentID(0)));
    m_HPWaterVolumeFilterShader->SetInt("u_VolumeColor", 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(inputFBO->GetColorAttachmentID(1)));
    m_HPWaterVolumeFilterShader->SetInt("u_VolumeTransmittance", 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(inputFBO->GetColorAttachmentID(2)));
    m_HPWaterVolumeFilterShader->SetInt("u_VolumeDepth", 2);

    m_HPWaterVolumeFilterShader->SetFloat("u_FilterStep", stride);
    m_HPWaterVolumeFilterShader->SetInt("u_SpatialDepthAwareEnabled",
        spatialDepthAwareEnabled ? 1 : 0);
    m_HPWaterVolumeFilterShader->SetFloat("u_SpatialDepthSensitivity",
        std::clamp(spatialDepthSensitivity, 0.0f, 1000.0f));

    glBindVertexArray(m_QuadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);

    outputFBO->Unbind();
    return true;
}

bool DeferredRenderer::FilterHPWaterVolume(bool spatialFilterEnabled,
                                           int iterations,
                                           bool spatialDepthAwareEnabled,
                                           float spatialDepthSensitivity) {
    m_HPWaterVolumeSpatialFilterEnabled = spatialFilterEnabled;
    m_HPWaterVolumeSpatialFilterIterations = spatialFilterEnabled
        ? static_cast<uint32_t>(std::clamp(iterations, 1, 3))
        : 0u;
    m_HPWaterVolumeSpatialDepthAwareEnabled = spatialDepthAwareEnabled;
    m_HPWaterVolumeSpatialDepthSensitivity = std::clamp(spatialDepthSensitivity, 0.0f, 1000.0f);

    if (!spatialFilterEnabled) {
        m_HPWaterVolumeFilteredValid = false;
        m_HPWaterVolumeFilterIterations = 0;
        return false;
    }

    if (!m_HPWaterVolumeFilterShader || !m_HPWaterVolumeFBO || !m_HPWaterVolumeFilteredFBO ||
        !m_HPWaterVolumeFilterScratchFBO || !m_HPWaterVolumeValid || m_QuadVAO == 0) {
        m_HPWaterVolumeFilteredValid = false;
        m_HPWaterVolumeFilterIterations = 0;
        return false;
    }

    m_HPWaterVolumeFilterIterations = 0;
    const auto filterInput = m_HPWaterVolumeTemporalValid && m_HPWaterVolumeTemporalFBO
        ? m_HPWaterVolumeTemporalFBO
        : m_HPWaterVolumeFBO;

    std::shared_ptr<Framebuffer> input = filterInput;
    std::shared_ptr<Framebuffer> output = m_HPWaterVolumeFilteredFBO;
    const float strides[] = { 1.0f, 2.0f, 4.0f };
    const uint32_t targetIterations = m_HPWaterVolumeSpatialFilterIterations;
    for (uint32_t i = 0; i < targetIterations; ++i) {
        if (!RunHPWaterVolumeFilterPass(input,
                                        output,
                                        strides[i],
                                        spatialDepthAwareEnabled,
                                        m_HPWaterVolumeSpatialDepthSensitivity)) {
            m_HPWaterVolumeFilteredValid = false;
            m_HPWaterVolumeFilterIterations = 0;
            return false;
        }
        ++m_HPWaterVolumeFilterIterations;
        input = output;
        output = output == m_HPWaterVolumeFilteredFBO
            ? m_HPWaterVolumeFilterScratchFBO
            : m_HPWaterVolumeFilteredFBO;
    }

    if (input != m_HPWaterVolumeFilteredFBO)
        std::swap(m_HPWaterVolumeFilteredFBO, m_HPWaterVolumeFilterScratchFBO);

    m_HPWaterVolumeFilteredValid = m_HPWaterVolumeFilterIterations == targetIterations;
    if (m_HPWaterVolumeFilteredValid)
        CommitHPWaterVolumeHistory();
    m_HPWaterVolumeUpsampledValid = false;
    return m_HPWaterVolumeFilteredValid;
}

bool DeferredRenderer::UpsampleHPWaterVolume(float nearClip, float farClip) {
    if (!m_HPWaterVolumeUpsampleShader || !m_HPWaterVolumeUpsampledFBO ||
        !m_HPWaterVolumeFBO || !m_HPWaterCompositeFBO || !m_HPWaterGBuffer || m_QuadVAO == 0) {
        m_HPWaterVolumeUpsampledValid = false;
        return false;
    }

    const auto inputFBO = m_HPWaterVolumeFilteredValid && m_HPWaterVolumeFilteredFBO
        ? m_HPWaterVolumeFilteredFBO
        : (m_HPWaterVolumeTemporalValid && m_HPWaterVolumeTemporalFBO
            ? m_HPWaterVolumeTemporalFBO
            : m_HPWaterVolumeFBO);

    if (!inputFBO || !m_HPWaterVolumeValid) {
        m_HPWaterVolumeUpsampledValid = false;
        return false;
    }

    m_HPWaterVolumeUpsampledFBO->Bind();
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

    m_HPWaterVolumeUpsampleShader->Bind();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(inputFBO->GetColorAttachmentID(0)));
    m_HPWaterVolumeUpsampleShader->SetInt("u_LowResVolumeColor", 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(inputFBO->GetColorAttachmentID(1)));
    m_HPWaterVolumeUpsampleShader->SetInt("u_LowResVolumeTransmittance", 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(inputFBO->GetColorAttachmentID(2)));
    m_HPWaterVolumeUpsampleShader->SetInt("u_LowResVolumeDepth", 2);

    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_HPWaterGBuffer->GetDepthAttachmentID()));
    m_HPWaterVolumeUpsampleShader->SetInt("u_HPWaterDepth", 3);

    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_HPWaterCompositeFBO->GetColorAttachmentID(2)));
    m_HPWaterVolumeUpsampleShader->SetInt("u_HPWaterRefractionMeta", 4);

    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, m_HPWaterMaskValid && m_HPWaterMaskFBO
        ? static_cast<GLuint>(m_HPWaterMaskFBO->GetColorAttachmentID())
        : static_cast<GLuint>(m_HPWaterGBuffer->GetDepthAttachmentID()));
    m_HPWaterVolumeUpsampleShader->SetInt("u_HPWaterMask", 5);

    m_HPWaterVolumeUpsampleShader->SetFloat("u_NearClip", nearClip);
    m_HPWaterVolumeUpsampleShader->SetFloat("u_FarClip", farClip);
    m_HPWaterVolumeUpsampleShader->SetInt("u_HPWaterMaskEnabled", m_HPWaterMaskValid ? 1 : 0);

    glBindVertexArray(m_QuadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);

    m_HPWaterVolumeUpsampledFBO->Unbind();
    m_HPWaterVolumeUpsampledValid = true;
    return true;
}

bool DeferredRenderer::AccumulateHPWaterCaustics(float nearClip,
                                                 float farClip,
                                                 const glm::vec3& lightDir,
                                                 const glm::vec3& lightColor,
                                                 float lightIntensity,
                                                 const glm::mat4& viewProjection,
                                                 const glm::mat4& inverseViewProjection,
                                                 const std::array<glm::mat4, 4>& waterCascadeVP,
                                                 const std::array<float, 4>& waterCascadeSplits,
                                                 uint32_t shadowDepthTextureArray,
                                                 uint32_t shadowDepthResolution,
                                                 uint32_t frameIndex,
                                                 float strength,
                                                 float scale,
                                                 float depthFade,
                                                 float transmittanceStrength,
                                                 float leakReduction,
                                                 float shadowAlphaClipThreshold,
                                                 float scatterBoost,
                                                 bool rgbDispersion,
                                                 float dispersionStrength) {
    if (!m_HPWaterCausticShader || !m_HPWaterCausticFBO || !m_GBuffer ||
        !m_HPWaterGBuffer || m_QuadVAO == 0) {
        m_HPWaterCausticValid = false;
        m_HPWaterCausticFilteredValid = false;
        m_HPWaterCausticComputeIrradianceValid = false;
        m_HPWaterCausticComputeIrradianceRan = false;
        m_HPWaterCausticComputeAtomicEnabled = false;
        m_HPWaterCausticFilterIterations = 0;
        m_HPWaterCausticAtlasConsumed = false;
        m_HPWaterCausticShadowDepthConsumed = false;
        m_HPWaterCausticRGBReceiverProjectionEnabled = false;
        m_HPWaterCausticExponentialLightStepsEnabled = false;
        m_HPWaterCausticFrameDitherEnabled = false;
        m_HPWaterCausticAtlasReceiverOutputEnabled = false;
        m_HPWaterCausticCascadeBlendEnabled = false;
        m_HPWaterCausticAtlasEdgeFilterEnabled = false;
        m_HPWaterCausticSpectralWeightingEnabled = false;
        return false;
    }

    if (strength <= 0.0001f || lightIntensity <= 0.0001f) {
        m_HPWaterCausticValid = false;
        m_HPWaterCausticFilteredValid = false;
        m_HPWaterCausticComputeIrradianceValid = false;
        m_HPWaterCausticComputeIrradianceRan = false;
        m_HPWaterCausticComputeAtomicEnabled = false;
        m_HPWaterCausticFilterIterations = 0;
        m_HPWaterCausticAtlasConsumed = false;
        m_HPWaterCausticShadowDepthConsumed = false;
        m_HPWaterCausticRGBReceiverProjectionEnabled = false;
        m_HPWaterCausticExponentialLightStepsEnabled = false;
        m_HPWaterCausticFrameDitherEnabled = false;
        m_HPWaterCausticAtlasReceiverOutputEnabled = false;
        m_HPWaterCausticCascadeBlendEnabled = false;
        m_HPWaterCausticAtlasEdgeFilterEnabled = false;
        m_HPWaterCausticSpectralWeightingEnabled = false;
        return false;
    }

    const bool computeIrradianceValid =
        RunHPWaterCausticComputeIrradiance(
            nearClip, farClip, lightDir, lightIntensity, viewProjection,
            inverseViewProjection, waterCascadeVP, waterCascadeSplits,
            shadowDepthTextureArray, shadowDepthResolution, frameIndex, strength, scale, depthFade,
            shadowAlphaClipThreshold, rgbDispersion, dispersionStrength);

    m_HPWaterCausticFBO->Bind();
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

    m_HPWaterCausticShader->Bind();

    BindHPWaterGBufferTextures(0);
    m_HPWaterCausticShader->SetInt("u_HPWaterNormalRoughness", 0);
    m_HPWaterCausticShader->SetInt("u_HPWaterScatterThickness", 1);
    m_HPWaterCausticShader->SetInt("u_HPWaterAbsorptionFoam", 2);

    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_HPWaterGBuffer->GetDepthAttachmentID()));
    m_HPWaterCausticShader->SetInt("u_HPWaterDepth", 3);

    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, m_HPWaterMaskValid && m_HPWaterMaskFBO
        ? static_cast<GLuint>(m_HPWaterMaskFBO->GetColorAttachmentID())
        : static_cast<GLuint>(m_HPWaterGBuffer->GetDepthAttachmentID()));
    m_HPWaterCausticShader->SetInt("u_HPWaterMask", 4);

    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_GBuffer->GetDepthAttachmentID()));
    m_HPWaterCausticShader->SetInt("u_SceneDepth", 5);

    const bool atlasValid = m_HPWaterCausticAtlasValid && m_HPWaterCausticAtlasFBO &&
        m_HPWaterCausticAtlasFBO->GetColorAttachmentID() != 0 &&
        m_HPWaterCausticAtlasFBO->GetDepthAttachmentID() != 0;
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D, atlasValid
        ? static_cast<GLuint>(m_HPWaterCausticAtlasFBO->GetColorAttachmentID())
        : 0);
    m_HPWaterCausticShader->SetInt("u_HPWaterCausticAtlas", 6);

    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D, atlasValid
        ? static_cast<GLuint>(m_HPWaterCausticAtlasFBO->GetDepthAttachmentID())
        : 0);
    m_HPWaterCausticShader->SetInt("u_HPWaterCausticAtlasDepth", 7);

    glActiveTexture(GL_TEXTURE8);
    glBindTexture(GL_TEXTURE_2D, computeIrradianceValid
        ? static_cast<GLuint>(m_HPWaterCausticComputeIrradianceTexture)
        : 0);
    m_HPWaterCausticShader->SetInt("u_HPWaterCausticComputeIrradiance", 8);

    m_HPWaterCausticShader->SetFloat("u_NearClip", nearClip);
    m_HPWaterCausticShader->SetFloat("u_FarClip", farClip);
    m_HPWaterCausticShader->SetVec3("u_LightDir", lightDir);
    m_HPWaterCausticShader->SetVec3("u_LightColor", lightColor);
    m_HPWaterCausticShader->SetFloat("u_LightIntensity", lightIntensity);
    m_HPWaterCausticShader->SetFloat("u_CausticStrength", std::clamp(strength, 0.0f, 8.0f));
    m_HPWaterCausticShader->SetFloat("u_CausticScale", std::clamp(scale, 0.1f, 128.0f));
    m_HPWaterCausticShader->SetFloat("u_CausticDepthFade", std::clamp(depthFade, 0.1f, 500.0f));
    m_HPWaterCausticShader->SetFloat("u_CausticTransmittanceStrength",
        std::clamp(transmittanceStrength, 0.0f, 8.0f));
    m_HPWaterCausticShader->SetFloat("u_CausticLeakReduction",
        std::clamp(leakReduction, 0.0f, 1.0f));
    m_HPWaterCausticShader->SetFloat("u_CausticShadowAlphaClipThreshold",
        std::clamp(shadowAlphaClipThreshold, 0.0f, 1.0f));
    m_HPWaterCausticShader->SetFloat("u_CausticScatterBoost",
        std::clamp(scatterBoost, 0.0f, 4.0f));
    m_HPWaterCausticShader->SetInt("u_CausticRGBDispersion", rgbDispersion ? 1 : 0);
    m_HPWaterCausticShader->SetFloat("u_CausticDispersionStrength",
        std::clamp(dispersionStrength, 0.0f, 2.0f));
    m_HPWaterCausticShader->SetInt("u_HPWaterMaskEnabled", m_HPWaterMaskValid ? 1 : 0);
    m_HPWaterCausticShader->SetInt("u_HPWaterCausticAtlasEnabled", atlasValid ? 1 : 0);
    m_HPWaterCausticShader->SetInt("u_HPWaterCausticComputeEnabled", computeIrradianceValid ? 1 : 0);
    m_HPWaterCausticShader->SetMat4("u_InverseViewProjection", inverseViewProjection);
    for (int i = 0; i < 4; ++i) {
        m_HPWaterCausticShader->SetMat4("u_WaterCascadeVP[" + std::to_string(i) + "]",
                                        waterCascadeVP[static_cast<size_t>(i)]);
        m_HPWaterCausticShader->SetFloat("u_WaterCascadeSplits[" + std::to_string(i) + "]",
                                         waterCascadeSplits[static_cast<size_t>(i)]);
    }
    m_HPWaterCausticShader->SetFloat("u_HPWaterCausticAtlasWidth", atlasValid
        ? static_cast<float>(m_HPWaterCausticAtlasFBO->GetWidth())
        : 0.0f);
    m_HPWaterCausticShader->SetFloat("u_HPWaterCausticAtlasHeight", atlasValid
        ? static_cast<float>(m_HPWaterCausticAtlasFBO->GetHeight())
        : 0.0f);

    glBindVertexArray(m_QuadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);

    m_HPWaterCausticFBO->Unbind();
    m_HPWaterCausticValid = true;
    m_HPWaterCausticFilteredValid = false;
    m_HPWaterCausticFilterIterations = 0;
    m_HPWaterCausticAtlasConsumed = atlasValid;
    m_HPWaterCausticCascadeBlendEnabled = computeIrradianceValid && atlasValid;
    return true;
}

bool DeferredRenderer::RunHPWaterCausticComputeIrradiance(float nearClip,
                                                          float farClip,
                                                          const glm::vec3& lightDir,
                                                          float lightIntensity,
                                                          const glm::mat4& viewProjection,
                                                          const glm::mat4& inverseViewProjection,
                                                          const std::array<glm::mat4, 4>& waterCascadeVP,
                                                          const std::array<float, 4>& waterCascadeSplits,
                                                          uint32_t shadowDepthTextureArray,
                                                          uint32_t shadowDepthResolution,
                                                          uint32_t frameIndex,
                                                          float strength,
                                                          float scale,
                                                          float depthFade,
                                                          float shadowAlphaClipThreshold,
                                                          bool rgbDispersion,
                                                          float dispersionStrength) {
    m_HPWaterCausticComputeIrradianceRan = false;
    m_HPWaterCausticComputeIrradianceValid = false;
    m_HPWaterCausticComputeAtomicEnabled = false;
    m_HPWaterCausticShadowDepthConsumed = false;
    m_HPWaterCausticRGBReceiverProjectionEnabled = false;
    m_HPWaterCausticExponentialLightStepsEnabled = false;
    m_HPWaterCausticFrameDitherEnabled = false;
    m_HPWaterCausticAtlasReceiverOutputEnabled = false;
    m_HPWaterCausticCascadeBlendEnabled = false;
    m_HPWaterCausticAtlasEdgeFilterEnabled = false;
    m_HPWaterCausticSpectralWeightingEnabled = false;

    if (!m_HPWaterCausticComputeShader || !m_HPWaterCausticResolveShader ||
        m_HPWaterCausticComputeIrradianceTexture == 0 ||
        m_HPWaterCausticComputeAtomicTextures[0] == 0 ||
        m_HPWaterCausticComputeAtomicTextures[1] == 0 ||
        m_HPWaterCausticComputeAtomicTextures[2] == 0 ||
        m_HPWaterCausticComputeAtomicTextures[3] == 0 ||
        !m_GBuffer || !m_HPWaterGBuffer || m_Width == 0 || m_Height == 0) {
        return false;
    }

    const bool atlasValid = m_HPWaterCausticAtlasValid && m_HPWaterCausticAtlasFBO &&
        m_HPWaterCausticAtlasFBO->GetColorAttachmentID() != 0 &&
        m_HPWaterCausticAtlasFBO->GetDepthAttachmentID() != 0;
    const bool shadowDepthValid = shadowDepthTextureArray != 0 && shadowDepthResolution > 0;
    const uint32_t outputWidth = atlasValid ? m_HPWaterCausticAtlasFBO->GetWidth() : m_Width;
    const uint32_t outputHeight = atlasValid ? m_HPWaterCausticAtlasFBO->GetHeight() : m_Height;
    if (m_HPWaterCausticComputeWidth != outputWidth ||
        m_HPWaterCausticComputeHeight != outputHeight) {
        CreateHPWaterCausticComputeTexture(outputWidth, outputHeight);
        if (m_HPWaterCausticComputeIrradianceTexture == 0 ||
            m_HPWaterCausticComputeAtomicTextures[0] == 0 ||
            m_HPWaterCausticComputeAtomicTextures[1] == 0 ||
            m_HPWaterCausticComputeAtomicTextures[2] == 0 ||
            m_HPWaterCausticComputeAtomicTextures[3] == 0) {
            return false;
        }
    }

    const GLuint clearValue = 0u;
    for (uint32_t texture : m_HPWaterCausticComputeAtomicTextures) {
        glClearTexImage(static_cast<GLuint>(texture),
                        0,
                        GL_RED_INTEGER,
                        GL_UNSIGNED_INT,
                        &clearValue);
    }
    glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    m_HPWaterCausticComputeShader->Bind();

    BindHPWaterGBufferTextures(0);
    m_HPWaterCausticComputeShader->SetInt("u_HPWaterNormalRoughness", 0);
    m_HPWaterCausticComputeShader->SetInt("u_HPWaterScatterThickness", 1);
    m_HPWaterCausticComputeShader->SetInt("u_HPWaterAbsorptionFoam", 2);

    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_HPWaterGBuffer->GetDepthAttachmentID()));
    m_HPWaterCausticComputeShader->SetInt("u_HPWaterDepth", 3);

    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, m_HPWaterMaskValid && m_HPWaterMaskFBO
        ? static_cast<GLuint>(m_HPWaterMaskFBO->GetColorAttachmentID())
        : static_cast<GLuint>(m_HPWaterGBuffer->GetDepthAttachmentID()));
    m_HPWaterCausticComputeShader->SetInt("u_HPWaterMask", 4);

    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_GBuffer->GetDepthAttachmentID()));
    m_HPWaterCausticComputeShader->SetInt("u_SceneDepth", 5);

    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D, atlasValid
        ? static_cast<GLuint>(m_HPWaterCausticAtlasFBO->GetColorAttachmentID())
        : 0);
    m_HPWaterCausticComputeShader->SetInt("u_HPWaterCausticAtlas", 6);

    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D, atlasValid
        ? static_cast<GLuint>(m_HPWaterCausticAtlasFBO->GetDepthAttachmentID())
        : 0);
    m_HPWaterCausticComputeShader->SetInt("u_HPWaterCausticAtlasDepth", 7);

    glActiveTexture(GL_TEXTURE8);
    glBindTexture(GL_TEXTURE_2D_ARRAY, shadowDepthValid
        ? static_cast<GLuint>(shadowDepthTextureArray)
        : 0);
    m_HPWaterCausticComputeShader->SetInt("u_ShadowDepthCascadeAtlas", 8);

    for (uint32_t channel = 0; channel < 4; ++channel) {
        glBindImageTexture(channel,
                           static_cast<GLuint>(m_HPWaterCausticComputeAtomicTextures[channel]),
                           0,
                           GL_FALSE,
                           0,
                           GL_READ_WRITE,
                           GL_R32UI);
    }

    m_HPWaterCausticComputeShader->SetInt("u_OutputWidth", static_cast<int>(m_HPWaterCausticComputeWidth));
    m_HPWaterCausticComputeShader->SetInt("u_OutputHeight", static_cast<int>(m_HPWaterCausticComputeHeight));
    m_HPWaterCausticComputeShader->SetInt("u_InputWidth", static_cast<int>(m_Width));
    m_HPWaterCausticComputeShader->SetInt("u_InputHeight", static_cast<int>(m_Height));
    m_HPWaterCausticComputeShader->SetInt("u_HPWaterMaskEnabled", m_HPWaterMaskValid ? 1 : 0);
    m_HPWaterCausticComputeShader->SetInt("u_HPWaterCausticAtlasEnabled", atlasValid ? 1 : 0);
    m_HPWaterCausticComputeShader->SetFloat("u_HPWaterCausticAtlasWidth", atlasValid
        ? static_cast<float>(m_HPWaterCausticAtlasFBO->GetWidth())
        : 0.0f);
    m_HPWaterCausticComputeShader->SetFloat("u_HPWaterCausticAtlasHeight", atlasValid
        ? static_cast<float>(m_HPWaterCausticAtlasFBO->GetHeight())
        : 0.0f);
    m_HPWaterCausticComputeShader->SetFloat("u_NearClip", nearClip);
    m_HPWaterCausticComputeShader->SetFloat("u_FarClip", farClip);
    m_HPWaterCausticComputeShader->SetVec3("u_LightDir", lightDir);
    m_HPWaterCausticComputeShader->SetFloat("u_LightIntensity", lightIntensity);
    m_HPWaterCausticComputeShader->SetMat4("u_ViewProjection", viewProjection);
    m_HPWaterCausticComputeShader->SetMat4("u_InverseViewProjection", inverseViewProjection);
    m_HPWaterCausticComputeShader->SetInt("u_ShadowDepthCascadeAtlasEnabled", shadowDepthValid ? 1 : 0);
    m_HPWaterCausticComputeShader->SetFloat("u_ShadowDepthResolution", shadowDepthValid
        ? static_cast<float>(shadowDepthResolution)
        : 0.0f);
    m_HPWaterCausticComputeShader->SetInt("u_FrameIndex", static_cast<int>(frameIndex & 0x7fffffffu));
    m_HPWaterCausticComputeShader->SetInt("u_EnableFrameDither", shadowDepthValid ? 1 : 0);
    m_HPWaterCausticComputeShader->SetInt("u_EnableExponentialLightSteps", shadowDepthValid ? 1 : 0);
    m_HPWaterCausticComputeShader->SetInt("u_EnableAtlasReceiverOutput",
        (atlasValid && shadowDepthValid) ? 1 : 0);
    for (int i = 0; i < 4; ++i) {
        m_HPWaterCausticComputeShader->SetMat4("u_WaterCascadeVP[" + std::to_string(i) + "]",
                                               waterCascadeVP[static_cast<size_t>(i)]);
        m_HPWaterCausticComputeShader->SetFloat("u_WaterCascadeSplits[" + std::to_string(i) + "]",
                                                waterCascadeSplits[static_cast<size_t>(i)]);
    }
    m_HPWaterCausticComputeShader->SetFloat("u_CausticStrength", std::clamp(strength, 0.0f, 8.0f));
    m_HPWaterCausticComputeShader->SetFloat("u_CausticScale", std::clamp(scale, 0.1f, 128.0f));
    m_HPWaterCausticComputeShader->SetFloat("u_CausticDepthFade", std::clamp(depthFade, 0.1f, 500.0f));
    m_HPWaterCausticComputeShader->SetFloat("u_CausticShadowAlphaClipThreshold",
        std::clamp(shadowAlphaClipThreshold, 0.0f, 1.0f));
    m_HPWaterCausticComputeShader->SetInt("u_CausticRGBDispersion", rgbDispersion ? 1 : 0);
    m_HPWaterCausticComputeShader->SetFloat("u_CausticDispersionStrength",
        std::clamp(dispersionStrength, 0.0f, 2.0f));

    m_HPWaterCausticComputeShader->Dispatch((m_Width + 15u) / 16u, (m_Height + 15u) / 16u, 1u);
    m_HPWaterCausticComputeShader->MemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    for (uint32_t channel = 0; channel < 4; ++channel) {
        glBindImageTexture(channel, 0, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
    }
    m_HPWaterCausticComputeShader->Unbind();

    m_HPWaterCausticResolveShader->Bind();
    for (uint32_t channel = 0; channel < 4; ++channel) {
        glActiveTexture(GL_TEXTURE0 + channel);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_HPWaterCausticComputeAtomicTextures[channel]));
    }
    m_HPWaterCausticResolveShader->SetInt("u_AtomicIrradianceR", 0);
    m_HPWaterCausticResolveShader->SetInt("u_AtomicIrradianceG", 1);
    m_HPWaterCausticResolveShader->SetInt("u_AtomicIrradianceB", 2);
    m_HPWaterCausticResolveShader->SetInt("u_AtomicIrradianceA", 3);
    glBindImageTexture(4,
                       static_cast<GLuint>(m_HPWaterCausticComputeIrradianceTexture),
                       0,
                       GL_FALSE,
                       0,
                       GL_WRITE_ONLY,
                       GL_RGBA16F);
    m_HPWaterCausticResolveShader->SetInt("u_OutputWidth", static_cast<int>(m_HPWaterCausticComputeWidth));
    m_HPWaterCausticResolveShader->SetInt("u_OutputHeight", static_cast<int>(m_HPWaterCausticComputeHeight));
    m_HPWaterCausticResolveShader->SetFloat("u_AtomicScale", 16384.0f);
    m_HPWaterCausticResolveShader->Dispatch(
        (m_HPWaterCausticComputeWidth + 15u) / 16u,
        (m_HPWaterCausticComputeHeight + 15u) / 16u,
        1u);
    m_HPWaterCausticResolveShader->MemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    glBindImageTexture(4, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    m_HPWaterCausticResolveShader->Unbind();

    m_HPWaterCausticComputeIrradianceRan = true;
    m_HPWaterCausticComputeIrradianceValid = true;
    m_HPWaterCausticComputeAtomicEnabled = true;
    m_HPWaterCausticShadowDepthConsumed = shadowDepthValid;
    m_HPWaterCausticRGBReceiverProjectionEnabled =
        rgbDispersion && shadowDepthValid && dispersionStrength > 0.0001f;
    m_HPWaterCausticExponentialLightStepsEnabled = shadowDepthValid;
    m_HPWaterCausticFrameDitherEnabled = shadowDepthValid;
    m_HPWaterCausticAtlasReceiverOutputEnabled = atlasValid && shadowDepthValid;
    m_HPWaterCausticAtlasEdgeFilterEnabled = atlasValid && shadowDepthValid;
    m_HPWaterCausticSpectralWeightingEnabled =
        rgbDispersion && dispersionStrength > 0.0001f;
    return true;
}

bool DeferredRenderer::RunHPWaterCausticFilterPass(const std::shared_ptr<Framebuffer>& inputFBO,
                                                   const std::shared_ptr<Framebuffer>& outputFBO,
                                                   float stride,
                                                   float radius,
                                                   float depthSigma,
                                                   float luminanceWeight) {
    if (!m_HPWaterCausticFilterShader || !inputFBO || !outputFBO || !m_HPWaterGBuffer || m_QuadVAO == 0)
        return false;

    outputFBO->Bind();
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

    m_HPWaterCausticFilterShader->Bind();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(inputFBO->GetColorAttachmentID()));
    m_HPWaterCausticFilterShader->SetInt("u_CausticInput", 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_HPWaterGBuffer->GetDepthAttachmentID()));
    m_HPWaterCausticFilterShader->SetInt("u_HPWaterDepth", 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_HPWaterMaskValid && m_HPWaterMaskFBO
        ? static_cast<GLuint>(m_HPWaterMaskFBO->GetColorAttachmentID())
        : static_cast<GLuint>(m_HPWaterGBuffer->GetDepthAttachmentID()));
    m_HPWaterCausticFilterShader->SetInt("u_HPWaterMask", 2);

    m_HPWaterCausticFilterShader->SetFloat("u_FilterStep", std::max(stride, 1.0f));
    m_HPWaterCausticFilterShader->SetFloat("u_FilterRadius", std::clamp(radius, 0.25f, 8.0f));
    m_HPWaterCausticFilterShader->SetFloat("u_DepthSigma", std::clamp(depthSigma, 0.00001f, 0.05f));
    m_HPWaterCausticFilterShader->SetFloat("u_LuminanceWeight", std::clamp(luminanceWeight, 0.0f, 128.0f));
    m_HPWaterCausticFilterShader->SetInt("u_HPWaterMaskEnabled", m_HPWaterMaskValid ? 1 : 0);

    glBindVertexArray(m_QuadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);

    outputFBO->Unbind();
    return true;
}

bool DeferredRenderer::RunHPWaterCausticFilterComputePass(const std::shared_ptr<Framebuffer>& inputFBO,
                                                          const std::shared_ptr<Framebuffer>& outputFBO,
                                                          float stride,
                                                          float radius,
                                                          float depthSigma,
                                                          float luminanceWeight) {
    if (!m_HPWaterCausticFilterComputeShader || !inputFBO || !outputFBO || !m_HPWaterGBuffer)
        return false;

    const GLuint outputTexture = static_cast<GLuint>(outputFBO->GetColorAttachmentID());
    if (outputTexture == 0)
        return false;

    m_HPWaterCausticFilterComputeShader->Bind();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(inputFBO->GetColorAttachmentID()));
    m_HPWaterCausticFilterComputeShader->SetInt("u_CausticInput", 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_HPWaterGBuffer->GetDepthAttachmentID()));
    m_HPWaterCausticFilterComputeShader->SetInt("u_HPWaterDepth", 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_HPWaterMaskValid && m_HPWaterMaskFBO
        ? static_cast<GLuint>(m_HPWaterMaskFBO->GetColorAttachmentID())
        : static_cast<GLuint>(m_HPWaterGBuffer->GetDepthAttachmentID()));
    m_HPWaterCausticFilterComputeShader->SetInt("u_HPWaterMask", 2);

    glBindImageTexture(0, outputTexture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);

    m_HPWaterCausticFilterComputeShader->SetInt("u_OutputWidth", static_cast<int>(m_Width));
    m_HPWaterCausticFilterComputeShader->SetInt("u_OutputHeight", static_cast<int>(m_Height));
    m_HPWaterCausticFilterComputeShader->SetFloat("u_FilterStep", std::max(stride, 1.0f));
    m_HPWaterCausticFilterComputeShader->SetFloat("u_FilterRadius", std::clamp(radius, 0.25f, 8.0f));
    m_HPWaterCausticFilterComputeShader->SetFloat("u_DepthSigma", std::clamp(depthSigma, 0.00001f, 0.05f));
    m_HPWaterCausticFilterComputeShader->SetFloat("u_LuminanceWeight", std::clamp(luminanceWeight, 0.0f, 128.0f));
    m_HPWaterCausticFilterComputeShader->SetInt("u_HPWaterMaskEnabled", m_HPWaterMaskValid ? 1 : 0);

    m_HPWaterCausticFilterComputeShader->Dispatch((m_Width + 15u) / 16u, (m_Height + 15u) / 16u, 1u);
    m_HPWaterCausticFilterComputeShader->MemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    m_HPWaterCausticFilterComputeShader->Unbind();
    return true;
}

bool DeferredRenderer::FilterHPWaterCaustics(float radius,
                                             float depthSigma,
                                             float luminanceWeight,
                                             int iterations) {
    if ((!m_HPWaterCausticFilterComputeShader && (!m_HPWaterCausticFilterShader || m_QuadVAO == 0)) ||
        !m_HPWaterCausticFBO ||
        !m_HPWaterCausticFilteredFBO || !m_HPWaterCausticFilterScratchFBO ||
        !m_HPWaterCausticValid) {
        m_HPWaterCausticFilteredValid = false;
        m_HPWaterCausticFilterIterations = 0;
        m_HPWaterCausticFilterComputeParityEnabled = false;
        m_HPWaterCausticFilterLDSHaloEnabled = false;
        return false;
    }

    m_HPWaterCausticFilterIterations = 0;
    m_HPWaterCausticFilterComputeParityEnabled = false;
    m_HPWaterCausticFilterLDSHaloEnabled = false;
    const int clampedIterations = std::clamp(iterations, 1, 2);
    std::shared_ptr<Framebuffer> inputFBO = m_HPWaterCausticFBO;
    for (int i = 0; i < clampedIterations; ++i) {
        const bool lastPass = i == clampedIterations - 1;
        const auto outputFBO = lastPass ? m_HPWaterCausticFilteredFBO : m_HPWaterCausticFilterScratchFBO;
        const float stride = static_cast<float>(1u << static_cast<uint32_t>(i));
        bool filtered = false;
        if (m_HPWaterCausticFilterComputeShader) {
            filtered = RunHPWaterCausticFilterComputePass(inputFBO,
                                                          outputFBO,
                                                          stride,
                                                          radius,
                                                          depthSigma,
                                                          luminanceWeight);
            if (filtered) {
                m_HPWaterCausticFilterComputeParityEnabled = true;
                m_HPWaterCausticFilterLDSHaloEnabled = true;
            }
        }
        if (!filtered) {
            filtered = RunHPWaterCausticFilterPass(inputFBO,
                                                   outputFBO,
                                                   stride,
                                                   radius,
                                                   depthSigma,
                                                   luminanceWeight);
        }
        if (!filtered) {
            m_HPWaterCausticFilteredValid = false;
            m_HPWaterCausticFilterComputeParityEnabled = false;
            m_HPWaterCausticFilterLDSHaloEnabled = false;
            return false;
        }
        ++m_HPWaterCausticFilterIterations;
        inputFBO = outputFBO;
    }

    m_HPWaterCausticFilteredValid = m_HPWaterCausticFilterIterations == static_cast<uint32_t>(clampedIterations);
    return m_HPWaterCausticFilteredValid;
}

bool DeferredRenderer::UpdateHPWaterFluidDynamics(uint32_t resolution,
                                                  float waveSpeed,
                                                  float damping,
                                                  float sourceU,
                                                  float sourceV,
                                                  float sourceIntensity,
                                                  float sourceRadiusPixels,
                                                  const glm::vec3& boxCenter,
                                                  const glm::vec3& boxSize) {
    if (!m_HPWaterFluidComputeShader && (!m_HPWaterFluidShader || m_QuadVAO == 0)) {
        m_HPWaterFluidValid = false;
        m_HPWaterFluidComputeRan = false;
        m_HPWaterFluidEdgeAbsorptionParityEnabled = false;
        m_HPWaterFluidSourceClampEnabled = false;
        m_HPWaterFluidWaveEquationParityEnabled = false;
        return false;
    }

    resolution = std::clamp(resolution, 16u, 1024u);
    CreateHPWaterFluidFBO(resolution);
    if (!m_HPWaterFluidCurrentFBO || !m_HPWaterFluidPreviousFBO || !m_HPWaterFluidNextFBO) {
        m_HPWaterFluidValid = false;
        m_HPWaterFluidComputeRan = false;
        m_HPWaterFluidEdgeAbsorptionParityEnabled = false;
        m_HPWaterFluidSourceClampEnabled = false;
        m_HPWaterFluidWaveEquationParityEnabled = false;
        return false;
    }

    if (m_HPWaterFluidComputeShader) {
        const GLuint currentTexture = static_cast<GLuint>(m_HPWaterFluidCurrentFBO->GetColorAttachmentID());
        const GLuint previousTexture = static_cast<GLuint>(m_HPWaterFluidPreviousFBO->GetColorAttachmentID());
        const GLuint nextTexture = static_cast<GLuint>(m_HPWaterFluidNextFBO->GetColorAttachmentID());
        const GLuint waterHeightTexture = static_cast<GLuint>(GetHPWaterFluidWaterHeightTexture());
        const GLuint sceneHeightTexture = static_cast<GLuint>(GetHPWaterFluidSceneHeightTexture());
        const bool heightFieldValid = IsHPWaterFluidHeightFieldValid();

        m_HPWaterFluidComputeShader->Bind();
        glBindImageTexture(0, currentTexture, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindImageTexture(1, previousTexture, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindImageTexture(2, nextTexture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D,
                      m_HPWaterFluidObstacleValid ? static_cast<GLuint>(m_HPWaterFluidObstacleTexture) : 0);
        m_HPWaterFluidComputeShader->SetInt("u_ObstacleMask", 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, heightFieldValid ? waterHeightTexture : 0);
        m_HPWaterFluidComputeShader->SetInt("u_WaterHeightTexture", 1);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, heightFieldValid ? sceneHeightTexture : 0);
        m_HPWaterFluidComputeShader->SetInt("u_SceneHeightTexture", 2);
        m_HPWaterFluidComputeShader->SetFloat("u_WaveSpeed", waveSpeed);
        m_HPWaterFluidComputeShader->SetFloat("u_DampingFactor", damping);
        m_HPWaterFluidComputeShader->SetFloat("u_WaveSourceIntensity", sourceIntensity);
        m_HPWaterFluidComputeShader->SetFloat("u_WaveSourceRadius", sourceRadiusPixels);
        m_HPWaterFluidComputeShader->SetVec3("u_WaveSourceUV", glm::vec3(sourceU, sourceV, 0.0f));
        m_HPWaterFluidComputeShader->SetInt("u_ObstacleMaskEnabled", m_HPWaterFluidObstacleValid ? 1 : 0);
        m_HPWaterFluidComputeShader->SetInt("u_HeightFieldEnabled", heightFieldValid ? 1 : 0);
        m_HPWaterFluidComputeShader->SetFloat("u_HeightObstacleEpsilon", 0.0f);

        const uint32_t groupCount = (resolution + 15u) / 16u;
        m_HPWaterFluidComputeShader->Dispatch(groupCount, groupCount, 1);
        m_HPWaterFluidComputeShader->MemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

        glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindImageTexture(1, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16F);
        glBindImageTexture(2, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);

        auto oldPrevious = m_HPWaterFluidPreviousFBO;
        m_HPWaterFluidPreviousFBO = m_HPWaterFluidCurrentFBO;
        m_HPWaterFluidCurrentFBO = m_HPWaterFluidNextFBO;
        m_HPWaterFluidNextFBO = oldPrevious;

        m_HPWaterFluidBoxCenter = boxCenter;
        m_HPWaterFluidBoxSize = glm::max(boxSize, glm::vec3(0.001f));
        m_HPWaterFluidValid = true;
        m_HPWaterFluidComputeRan = true;
        m_HPWaterFluidEdgeAbsorptionParityEnabled = true;
        m_HPWaterFluidSourceClampEnabled = true;
        m_HPWaterFluidWaveEquationParityEnabled = true;
        return true;
    }

    GLint previousViewport[4] = {};
    glGetIntegerv(GL_VIEWPORT, previousViewport);
    GLboolean previousDepthMask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &previousDepthMask);
    const bool previousDepthTest = glIsEnabled(GL_DEPTH_TEST) == GL_TRUE;

    m_HPWaterFluidNextFBO->Bind();
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

    m_HPWaterFluidShader->Bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_HPWaterFluidCurrentFBO->GetColorAttachmentID()));
    m_HPWaterFluidShader->SetInt("u_WaveHeightCurrent", 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_HPWaterFluidPreviousFBO->GetColorAttachmentID()));
    m_HPWaterFluidShader->SetInt("u_WaveHeightPrevious", 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D,
                  m_HPWaterFluidObstacleValid ? static_cast<GLuint>(m_HPWaterFluidObstacleTexture) : 0);
    m_HPWaterFluidShader->SetInt("u_ObstacleMask", 2);

    const GLuint waterHeightTexture = static_cast<GLuint>(GetHPWaterFluidWaterHeightTexture());
    const GLuint sceneHeightTexture = static_cast<GLuint>(GetHPWaterFluidSceneHeightTexture());
    const bool heightFieldValid = IsHPWaterFluidHeightFieldValid();

    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, heightFieldValid ? waterHeightTexture : 0);
    m_HPWaterFluidShader->SetInt("u_WaterHeightTexture", 3);

    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, heightFieldValid ? sceneHeightTexture : 0);
    m_HPWaterFluidShader->SetInt("u_SceneHeightTexture", 4);

    m_HPWaterFluidShader->SetFloat("u_WaveSpeed", waveSpeed);
    m_HPWaterFluidShader->SetFloat("u_DampingFactor", damping);
    m_HPWaterFluidShader->SetFloat("u_WaveSourceIntensity", sourceIntensity);
    m_HPWaterFluidShader->SetFloat("u_WaveSourceRadius", sourceRadiusPixels);
    m_HPWaterFluidShader->SetVec3("u_WaveSourceUV", glm::vec3(sourceU, sourceV, 0.0f));
    m_HPWaterFluidShader->SetInt("u_ObstacleMaskEnabled", m_HPWaterFluidObstacleValid ? 1 : 0);
    m_HPWaterFluidShader->SetInt("u_HeightFieldEnabled", heightFieldValid ? 1 : 0);
    m_HPWaterFluidShader->SetFloat("u_HeightObstacleEpsilon", 0.0f);

    glBindVertexArray(m_QuadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    if (previousDepthTest)
        glEnable(GL_DEPTH_TEST);
    else
        glDisable(GL_DEPTH_TEST);
    glDepthMask(previousDepthMask);
    glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);

    m_HPWaterFluidNextFBO->Unbind();

    auto oldPrevious = m_HPWaterFluidPreviousFBO;
    m_HPWaterFluidPreviousFBO = m_HPWaterFluidCurrentFBO;
    m_HPWaterFluidCurrentFBO = m_HPWaterFluidNextFBO;
    m_HPWaterFluidNextFBO = oldPrevious;

    m_HPWaterFluidBoxCenter = boxCenter;
    m_HPWaterFluidBoxSize = glm::max(boxSize, glm::vec3(0.001f));
    m_HPWaterFluidValid = true;
    m_HPWaterFluidComputeRan = false;
    m_HPWaterFluidEdgeAbsorptionParityEnabled = true;
    m_HPWaterFluidSourceClampEnabled = true;
    m_HPWaterFluidWaveEquationParityEnabled = true;
    return true;
}

uint32_t DeferredRenderer::GetOutputTexture() const {
    if (m_HPWaterCompositeValid && m_HPWaterCompositeFBO)
        return static_cast<uint32_t>(m_HPWaterCompositeFBO->GetColorAttachmentID());
    if (!m_LightingFBO) return 0;
    return static_cast<uint32_t>(m_LightingFBO->GetColorAttachmentID());
}

uint32_t DeferredRenderer::GetLightingTexture() const {
    if (!m_LightingFBO) return 0;
    return static_cast<uint32_t>(m_LightingFBO->GetColorAttachmentID());
}

uint32_t DeferredRenderer::GetHPWaterCompositeTexture() const {
    if (!m_HPWaterCompositeFBO) return 0;
    return static_cast<uint32_t>(m_HPWaterCompositeFBO->GetColorAttachmentID());
}

uint32_t DeferredRenderer::GetHPWaterRefractionDataTexture() const {
    if (!m_HPWaterCompositeFBO || m_HPWaterCompositeFBO->GetColorAttachmentCount() < 2) return 0;
    return static_cast<uint32_t>(m_HPWaterCompositeFBO->GetColorAttachmentID(1));
}

uint32_t DeferredRenderer::GetHPWaterRefractionMetaTexture() const {
    if (!m_HPWaterCompositeFBO || m_HPWaterCompositeFBO->GetColorAttachmentCount() < 3) return 0;
    return static_cast<uint32_t>(m_HPWaterCompositeFBO->GetColorAttachmentID(2));
}

uint32_t DeferredRenderer::GetHPWaterSSRDiagnosticsTexture() const {
    if (!m_HPWaterCompositeFBO || m_HPWaterCompositeFBO->GetColorAttachmentCount() < 4) return 0;
    return static_cast<uint32_t>(m_HPWaterCompositeFBO->GetColorAttachmentID(3));
}

uint32_t DeferredRenderer::GetHPWaterMaskTexture() const {
    if (!m_HPWaterMaskFBO) return 0;
    return static_cast<uint32_t>(m_HPWaterMaskFBO->GetColorAttachmentID());
}

uint32_t DeferredRenderer::GetHPWaterVolumeTexture(int index) const {
    if (!m_HPWaterVolumeFBO) return 0;
    return static_cast<uint32_t>(m_HPWaterVolumeFBO->GetColorAttachmentID(index));
}

uint32_t DeferredRenderer::GetHPWaterVolumeMotionVectorTexture() const {
    if (!m_HPWaterVolumeMotionVectorFBO) return 0;
    return static_cast<uint32_t>(m_HPWaterVolumeMotionVectorFBO->GetColorAttachmentID());
}

uint32_t DeferredRenderer::GetHPWaterVolumeFilteredTexture(int index) const {
    if (!m_HPWaterVolumeFilteredFBO) return 0;
    return static_cast<uint32_t>(m_HPWaterVolumeFilteredFBO->GetColorAttachmentID(index));
}

uint32_t DeferredRenderer::GetHPWaterVolumeHistoryTexture(int index) const {
    if (!m_HPWaterVolumeHistoryFBO) return 0;
    return static_cast<uint32_t>(m_HPWaterVolumeHistoryFBO->GetColorAttachmentID(index));
}

uint32_t DeferredRenderer::GetHPWaterVolumeUpsampledTexture(int index) const {
    if (!m_HPWaterVolumeUpsampledFBO) return 0;
    return static_cast<uint32_t>(m_HPWaterVolumeUpsampledFBO->GetColorAttachmentID(index));
}

uint32_t DeferredRenderer::GetHPWaterCausticTexture() const {
    if (!m_HPWaterCausticFBO) return 0;
    return static_cast<uint32_t>(m_HPWaterCausticFBO->GetColorAttachmentID());
}

uint32_t DeferredRenderer::GetHPWaterCausticFilteredTexture() const {
    if (!m_HPWaterCausticFilteredFBO) return 0;
    return static_cast<uint32_t>(m_HPWaterCausticFilteredFBO->GetColorAttachmentID());
}

uint32_t DeferredRenderer::GetHPWaterCausticAtlasTexture() const {
    if (!m_HPWaterCausticAtlasFBO) return 0;
    return static_cast<uint32_t>(m_HPWaterCausticAtlasFBO->GetColorAttachmentID());
}

uint32_t DeferredRenderer::GetHPWaterCausticAtlasDepthTexture() const {
    if (!m_HPWaterCausticAtlasFBO) return 0;
    return static_cast<uint32_t>(m_HPWaterCausticAtlasFBO->GetDepthAttachmentID());
}

uint32_t DeferredRenderer::GetHPWaterCausticAtlasWidth() const {
    if (!m_HPWaterCausticAtlasFBO) return 0;
    return m_HPWaterCausticAtlasFBO->GetWidth();
}

uint32_t DeferredRenderer::GetHPWaterCausticAtlasHeight() const {
    if (!m_HPWaterCausticAtlasFBO) return 0;
    return m_HPWaterCausticAtlasFBO->GetHeight();
}

uint32_t DeferredRenderer::GetHPWaterFluidHeightTexture() const {
    if (!m_HPWaterFluidCurrentFBO) return 0;
    return static_cast<uint32_t>(m_HPWaterFluidCurrentFBO->GetColorAttachmentID());
}

bool DeferredRenderer::UpdateHPWaterSpectrumTexture(uint32_t resolution,
                                                    const glm::vec3& boxSize,
                                                    bool enabled,
                                                    float amplitude,
                                                    float windAngle,
                                                    float time,
                                                    float normalStrength,
                                                    float choppiness) {
    m_HPWaterSpectrumComputeRan = false;
    m_HPWaterSpectrumComputeValid = false;

    if (!m_HPWaterSpectrumComputeShader || !enabled || amplitude <= 0.0f)
        return false;

    const uint32_t safeResolution = std::clamp(resolution, 16u, 2048u);
    CreateHPWaterSpectrumTexture(safeResolution);
    if (m_HPWaterSpectrumTexture == 0)
        return false;

    m_HPWaterSpectrumComputeShader->Bind();
    glBindImageTexture(0, static_cast<GLuint>(m_HPWaterSpectrumTexture), 0, GL_FALSE, 0,
                       GL_WRITE_ONLY, GL_RGBA16F);
    m_HPWaterSpectrumComputeShader->SetVec3("u_BoxSize", glm::max(boxSize, glm::vec3(0.001f)));
    m_HPWaterSpectrumComputeShader->SetFloat("u_Amplitude", std::max(amplitude, 0.0f));
    m_HPWaterSpectrumComputeShader->SetFloat("u_WindAngle", windAngle);
    m_HPWaterSpectrumComputeShader->SetFloat("u_Time", time);
    m_HPWaterSpectrumComputeShader->SetFloat("u_NormalStrength", std::clamp(normalStrength, 0.0f, 4.0f));
    m_HPWaterSpectrumComputeShader->SetFloat("u_Choppiness", std::clamp(choppiness, 0.0f, 4.0f));
    m_HPWaterSpectrumComputeShader->SetInt("u_Enabled", 1);
    m_HPWaterSpectrumComputeShader->Dispatch((safeResolution + 15u) / 16u,
                                             (safeResolution + 15u) / 16u,
                                             1u);
    m_HPWaterSpectrumComputeShader->MemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    m_HPWaterSpectrumComputeShader->Unbind();

    m_HPWaterSpectrumComputeRan = true;
    m_HPWaterSpectrumComputeValid = true;
    return true;
}

uint32_t DeferredRenderer::GetHPWaterFluidWaterHeightTexture() const {
    if (m_HPWaterFluidHeightCaptureValid && m_HPWaterFluidWaterHeightFBO)
        return static_cast<uint32_t>(m_HPWaterFluidWaterHeightFBO->GetColorAttachmentID());
    return m_HPWaterFluidWaterHeightTexture;
}

uint32_t DeferredRenderer::GetHPWaterFluidSceneHeightTexture() const {
    if (m_HPWaterFluidHeightCaptureValid && m_HPWaterFluidSceneHeightFBO)
        return static_cast<uint32_t>(m_HPWaterFluidSceneHeightFBO->GetColorAttachmentID());
    return m_HPWaterFluidSceneHeightTexture;
}

uint32_t DeferredRenderer::GetHPWaterVolumeWidth() const {
    if (!m_HPWaterVolumeFBO) return 0;
    return m_HPWaterVolumeFBO->GetWidth();
}

uint32_t DeferredRenderer::GetHPWaterVolumeHeight() const {
    if (!m_HPWaterVolumeFBO) return 0;
    return m_HPWaterVolumeFBO->GetHeight();
}

uint32_t DeferredRenderer::GetDepthTexture() const {
    if (!m_GBuffer) return 0;
    return static_cast<uint32_t>(m_GBuffer->GetDepthAttachmentID());
}

uint32_t DeferredRenderer::GetHPWaterGBufferTexture(int index) const {
    if (!m_HPWaterGBuffer) return 0;
    return static_cast<uint32_t>(m_HPWaterGBuffer->GetColorAttachmentID(index));
}

uint32_t DeferredRenderer::GetHPWaterDepthTexture() const {
    if (!m_HPWaterGBuffer) return 0;
    return static_cast<uint32_t>(m_HPWaterGBuffer->GetDepthAttachmentID());
}

int DeferredRenderer::GetHPWaterGBufferAttachmentCount() const {
    if (!m_HPWaterGBuffer) return 0;
    return m_HPWaterGBuffer->GetColorAttachmentCount();
}

void DeferredRenderer::BindHPWaterGBufferTextures(int startUnit) {
    if (!m_HPWaterGBuffer) return;

    for (int i = 0; i < m_HPWaterGBuffer->GetColorAttachmentCount(); ++i) {
        glActiveTexture(GL_TEXTURE0 + startUnit + i);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_HPWaterGBuffer->GetColorAttachmentID(i)));
    }
}

void DeferredRenderer::BeginForwardPass() {
    if (!m_LightingFBO || !m_GBuffer) return;

    m_LightingFBO->Bind();
    GLint targetFBO = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &targetFBO);

    BlitDepthTo(static_cast<uint32_t>(targetFBO), m_Width, m_Height);
    m_LightingFBO->Bind();

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void DeferredRenderer::EndForwardPass() {
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
    if (m_LightingFBO)
        m_LightingFBO->Unbind();
}

void DeferredRenderer::BlitDepthTo(uint32_t targetFBO, uint32_t width, uint32_t height) {
    if (!m_GBuffer) return;

    // Get the G-buffer's internal FBO ID
    // We need to blit from the G-buffer's depth to the target
    // The G-buffer FBO is bound when we call Bind(), so we use the same approach
    m_GBuffer->Bind(); // sets read to G-buffer
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0); // unbind first
    m_GBuffer->Bind();

    // Read from G-buffer, write to target
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, targetFBO);

    // Blit only the depth buffer
    glBlitFramebuffer(0, 0, m_Width, m_Height,
                      0, 0, width, height,
                      GL_DEPTH_BUFFER_BIT, GL_NEAREST);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void DeferredRenderer::DebugVisualize(GBufferDebugView view) {
    if (!m_DebugShader || !m_LightingFBO || !m_GBuffer) return;
    if (view == GBufferDebugView::None) return;
    m_HPWaterCompositeValid = false;

    m_LightingFBO->Bind();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

    m_DebugShader->Bind();

    // Bind G-buffer textures
    BindGBufferTextures(0);
    m_DebugShader->SetInt("u_GPositionMetallic", 0);
    m_DebugShader->SetInt("u_GNormalRoughness", 1);
    m_DebugShader->SetInt("u_GAlbedoAO", 2);
    m_DebugShader->SetInt("u_GEmissionFlags", 3);

    // Bind depth texture
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_GBuffer->GetDepthAttachmentID()));
    m_DebugShader->SetInt("u_DepthTexture", 4);

    BindHPWaterGBufferTextures(5);
    m_DebugShader->SetInt("u_HPWaterNormalRoughness", 5);
    m_DebugShader->SetInt("u_HPWaterScatterThickness", 6);
    m_DebugShader->SetInt("u_HPWaterAbsorptionFoam", 7);

    m_DebugShader->SetInt("u_DebugMode", static_cast<int>(view));

    // Draw fullscreen triangle
    glBindVertexArray(m_QuadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);

    m_LightingFBO->Unbind();
}

} // namespace VE
