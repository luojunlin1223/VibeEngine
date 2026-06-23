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

namespace VE {

static uint32_t CalculateMipCount(uint32_t width, uint32_t height) {
    uint32_t maxDim = std::max(width, height);
    uint32_t mipCount = 1;
    while (maxDim > 1) {
        maxDim /= 2;
        ++mipCount;
    }
    return mipCount;
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

    m_HPWaterFluidComputeShader = ComputeShader::CreateFromFile("shaders/HPWaterFluidDynamics.comp");
    if (!m_HPWaterFluidComputeShader) {
        VE_ENGINE_ERROR("DeferredRenderer: Failed to load HPWaterFluidDynamics.comp");
    }

    m_HPWaterDepthPyramidShader = Shader::CreateFromFile("shaders/HPWaterDepthPyramid.shader");
    if (!m_HPWaterDepthPyramidShader) {
        VE_ENGINE_ERROR("DeferredRenderer: Failed to load HPWaterDepthPyramid.shader");
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

    m_GBuffer.reset();
    m_LightingFBO.reset();
    m_HPWaterGBuffer.reset();
    m_HPWaterMaskFBO.reset();
    m_HPWaterCompositeFBO.reset();
    m_HPWaterVolumeFBO.reset();
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
    m_HPWaterVolumeTemporalShader.reset();
    m_HPWaterVolumeFilterShader.reset();
    m_HPWaterVolumeUpsampleShader.reset();
    m_HPWaterCausticShader.reset();
    m_HPWaterCausticFilterShader.reset();
    m_HPWaterCausticAtlasShader.reset();
    m_HPWaterCausticComputeShader.reset();
    m_HPWaterFluidComputeShader.reset();
    m_HPWaterDepthPyramidShader.reset();
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
    m_HPWaterCausticValid = false;
    m_HPWaterCausticFilteredValid = false;
    m_HPWaterCausticComputeIrradianceValid = false;
    m_HPWaterCausticComputeIrradianceRan = false;
    m_HPWaterCausticAtlasValid = false;
    m_HPWaterCausticAtlasConsumed = false;
    m_HPWaterCausticAtlasTileResolution = 0;
    m_HPWaterDepthPyramidValid = false;
    m_HPWaterFluidValid = false;
    m_HPWaterFluidInitialized = false;
    m_HPWaterFluidComputeRan = false;
    m_HPWaterFluidObstacleValid = false;
    m_HPWaterFluidHeightFieldValid = false;
    m_HPWaterFluidHeightCaptureRan = false;
    m_HPWaterFluidHeightCaptureValid = false;
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
    };
    m_HPWaterCompositeFBO = Framebuffer::Create(compositeSpec);
    m_HPWaterCompositeValid = false;
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
    m_HPWaterCausticComputeIrradianceValid = false;
    m_HPWaterCausticComputeIrradianceRan = false;
}

void DeferredRenderer::CreateHPWaterCausticComputeTexture() {
    DestroyHPWaterCausticComputeTexture();
    if (m_Width == 0 || m_Height == 0)
        return;

    glGenTextures(1, &m_HPWaterCausticComputeIrradianceTexture);
    VE_GPU_TRACK(GPUResourceType::Texture, m_HPWaterCausticComputeIrradianceTexture);
    glBindTexture(GL_TEXTURE_2D, m_HPWaterCausticComputeIrradianceTexture);
    glTexStorage2D(GL_TEXTURE_2D,
                   1,
                   GL_R16F,
                   static_cast<GLsizei>(m_Width),
                   static_cast<GLsizei>(m_Height));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    m_HPWaterCausticComputeIrradianceValid = false;
    m_HPWaterCausticComputeIrradianceRan = false;
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
    m_HPWaterFluidInitialized = true;
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
    m_HPWaterCausticValid = false;
    m_HPWaterCausticFilteredValid = false;
    m_HPWaterCausticComputeIrradianceValid = false;
    m_HPWaterCausticComputeIrradianceRan = false;
    m_HPWaterCausticAtlasConsumed = false;
    m_HPWaterDepthPyramidValid = false;
    m_HPWaterVolumeFilterIterations = 0;
    m_HPWaterCausticFilterIterations = 0;
}

void DeferredRenderer::ClearHPWaterGBuffer() {
    if (!m_HPWaterGBuffer) return;

    m_HPWaterGBuffer->Bind();
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    m_HPWaterMaskValid = false;
    m_HPWaterCausticAtlasValid = false;
    m_HPWaterCausticAtlasConsumed = false;
    m_HPWaterCausticComputeIrradianceValid = false;
    m_HPWaterCausticComputeIrradianceRan = false;
}

void DeferredRenderer::BeginGeometryPass() {
    if (!m_GBuffer) return;

    ClearHPWaterGBuffer();

    m_GBuffer->Bind();

    // Clear all G-buffer attachments to zero
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

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
    m_HPWaterCausticValid = false;
    m_HPWaterCausticFilteredValid = false;
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
                                        float maxRefractionCrossDistance,
                                        float refractionThicknessOffset,
                                        int refractionSampleCount,
                                        bool refractionJitter,
                                        uint32_t frameIndex,
                                        float environmentReflectionIntensity,
                                        float thinSSSStrength,
                                        float backlitTransmissionStrength,
                                        float forwardScatterStrength,
                                        float specularFGDStrength,
                                        float ggxEnergyCompensation,
                                        const glm::vec3& cameraPosition,
                                        const glm::vec3& lightDir,
                                        const glm::vec3& lightColor,
                                        float lightIntensity,
                                        const glm::vec3& indirectSkyColor,
                                        const glm::vec3& indirectGroundColor,
                                        const glm::vec3& indirectTint,
                                        bool indirectLightingEnabled,
                                        float indirectDiffuseIntensity,
                                        float skyReflectionIntensity,
                                        uint32_t skyTexture,
                                        uint32_t reflectionProbeTexture,
                                        bool hasReflectionProbe,
                                        float reflectionProbeIntensity,
                                        const glm::mat4& inverseViewProjection) {
    if (!m_HPWaterCompositeShader || !m_HPWaterCompositeFBO || !m_LightingFBO ||
        !m_GBuffer || !m_HPWaterGBuffer || m_QuadVAO == 0) {
        m_HPWaterCompositeValid = false;
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
    glActiveTexture(GL_TEXTURE13);
    glBindTexture(GL_TEXTURE_CUBE_MAP, reflectionProbeValid ? static_cast<GLuint>(reflectionProbeTexture) : 0);
    m_HPWaterCompositeShader->SetInt("u_ReflectionProbe", 13);
    m_HPWaterCompositeShader->SetInt("u_HasReflectionProbe", reflectionProbeValid ? 1 : 0);
    m_HPWaterCompositeShader->SetFloat("u_ReflectionProbeIntensity",
        reflectionProbeValid ? std::clamp(reflectionProbeIntensity, 0.0f, 4.0f) : 0.0f);

    m_HPWaterCompositeShader->SetFloat("u_NearClip", nearClip);
    m_HPWaterCompositeShader->SetFloat("u_FarClip", farClip);
    m_HPWaterCompositeShader->SetFloat("u_RefractionStrength", std::clamp(refractionStrength, 0.0f, 2.0f));
    m_HPWaterCompositeShader->SetFloat("u_MaxRefractionCrossDistance",
        std::clamp(maxRefractionCrossDistance, 0.1f, 200.0f));
    m_HPWaterCompositeShader->SetFloat("u_RefractionThicknessOffset",
        std::clamp(refractionThicknessOffset, 0.01f, 8.0f));
    m_HPWaterCompositeShader->SetInt("u_RefractionSampleCount", std::clamp(refractionSampleCount, 4, 64));
    m_HPWaterCompositeShader->SetInt("u_RefractionJitterEnabled", refractionJitter ? 1 : 0);
    m_HPWaterCompositeShader->SetInt("u_FrameIndex", static_cast<int>(frameIndex & 0x7fffffffU));
    m_HPWaterCompositeShader->SetFloat("u_EnvironmentReflectionIntensity",
        std::clamp(environmentReflectionIntensity, 0.0f, 3.0f));
    m_HPWaterCompositeShader->SetFloat("u_ThinSSSStrength",
        std::clamp(thinSSSStrength, 0.0f, 3.0f));
    m_HPWaterCompositeShader->SetFloat("u_BacklitTransmissionStrength",
        std::clamp(backlitTransmissionStrength, 0.0f, 3.0f));
    m_HPWaterCompositeShader->SetFloat("u_ForwardScatterStrength",
        std::clamp(forwardScatterStrength, 0.0f, 3.0f));
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
    m_HPWaterCompositeShader->SetVec3("u_IndirectSkyColor", indirectSkyColor);
    m_HPWaterCompositeShader->SetVec3("u_IndirectGroundColor", indirectGroundColor);
    m_HPWaterCompositeShader->SetVec3("u_IndirectTint", indirectTint);
    m_HPWaterCompositeShader->SetInt("u_IndirectLightingEnabled", indirectLightingEnabled ? 1 : 0);
    m_HPWaterCompositeShader->SetFloat("u_IndirectDiffuseIntensity",
        std::clamp(indirectDiffuseIntensity, 0.0f, 4.0f));
    m_HPWaterCompositeShader->SetFloat("u_SkyReflectionIntensity",
        std::clamp(skyReflectionIntensity, 0.0f, 4.0f));
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
    return true;
}

bool DeferredRenderer::AccumulateHPWaterVolume(float nearClip,
                                               float farClip,
                                               const glm::vec3& lightDir,
                                               const glm::vec3& lightColor,
                                               float lightIntensity,
                                               const glm::vec3& cameraPosition,
                                               const glm::mat4& inverseViewProjection,
                                               float macroScatterStrength,
                                               float causticVolumeStrength) {
    if (!m_HPWaterVolumeShader || !m_HPWaterVolumeFBO || !m_HPWaterCompositeFBO ||
        !m_GBuffer || !m_HPWaterGBuffer || m_QuadVAO == 0) {
        m_HPWaterVolumeValid = false;
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

    m_HPWaterVolumeShader->SetFloat("u_NearClip", nearClip);
    m_HPWaterVolumeShader->SetFloat("u_FarClip", farClip);
    m_HPWaterVolumeShader->SetVec3("u_LightDir", lightDir);
    m_HPWaterVolumeShader->SetVec3("u_LightColor", lightColor);
    m_HPWaterVolumeShader->SetFloat("u_LightIntensity", lightIntensity);
    m_HPWaterVolumeShader->SetVec3("u_CameraPosition", cameraPosition);
    m_HPWaterVolumeShader->SetMat4("u_InverseViewProjection", inverseViewProjection);
    m_HPWaterVolumeShader->SetFloat("u_MacroScatterStrength",
        std::clamp(macroScatterStrength, 0.0f, 4.0f));
    m_HPWaterVolumeShader->SetInt("u_HPWaterMaskEnabled", m_HPWaterMaskValid ? 1 : 0);
    m_HPWaterVolumeShader->SetInt("u_HPWaterCausticEnabled", causticVolumeValid ? 1 : 0);
    m_HPWaterVolumeShader->SetFloat("u_CausticVolumeStrength",
        std::clamp(causticVolumeStrength, 0.0f, 4.0f));

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
    return true;
}

bool DeferredRenderer::TemporalFilterHPWaterVolume(const glm::mat4& currentViewProjection,
                                                   const glm::mat4& previousViewProjection) {
    if (!m_HPWaterVolumeTemporalShader || !m_HPWaterVolumeFBO || !m_HPWaterVolumeTemporalFBO ||
        !m_HPWaterVolumeHistoryFBO || !m_HPWaterCompositeFBO || !m_HPWaterVolumeValid || m_QuadVAO == 0) {
        m_HPWaterVolumeTemporalValid = false;
        return false;
    }

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

    m_HPWaterVolumeTemporalShader->SetMat4("u_CurrentViewProjection", currentViewProjection);
    m_HPWaterVolumeTemporalShader->SetMat4("u_PreviousViewProjection", previousViewProjection);
    m_HPWaterVolumeTemporalShader->SetInt("u_HistoryValid", m_HPWaterVolumeHistoryValid ? 1 : 0);
    m_HPWaterVolumeTemporalShader->SetFloat("u_HistoryBlend", 0.88f);
    m_HPWaterVolumeTemporalShader->SetFloat("u_DepthRejectionThreshold", 0.035f);
    m_HPWaterVolumeTemporalShader->SetFloat("u_VelocityRejectionScale", 9.0f);

    glBindVertexArray(m_QuadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);

    m_HPWaterVolumeTemporalFBO->Unbind();
    m_HPWaterVolumeTemporalValid = true;
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
}

bool DeferredRenderer::RunHPWaterVolumeFilterPass(const std::shared_ptr<Framebuffer>& inputFBO,
                                                  const std::shared_ptr<Framebuffer>& outputFBO,
                                                  float stride) {
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

    glBindVertexArray(m_QuadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);

    outputFBO->Unbind();
    return true;
}

bool DeferredRenderer::FilterHPWaterVolume() {
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

    if (!RunHPWaterVolumeFilterPass(filterInput, m_HPWaterVolumeFilteredFBO, 1.0f))
        return false;
    ++m_HPWaterVolumeFilterIterations;

    if (!RunHPWaterVolumeFilterPass(m_HPWaterVolumeFilteredFBO, m_HPWaterVolumeFilterScratchFBO, 2.0f))
        return false;
    ++m_HPWaterVolumeFilterIterations;

    if (!RunHPWaterVolumeFilterPass(m_HPWaterVolumeFilterScratchFBO, m_HPWaterVolumeFilteredFBO, 4.0f))
        return false;
    ++m_HPWaterVolumeFilterIterations;

    m_HPWaterVolumeFilteredValid = m_HPWaterVolumeFilterIterations == 3;
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
                                                 float strength,
                                                 float scale,
                                                 float depthFade,
                                                 bool rgbDispersion,
                                                 float dispersionStrength) {
    if (!m_HPWaterCausticShader || !m_HPWaterCausticFBO || !m_GBuffer ||
        !m_HPWaterGBuffer || m_QuadVAO == 0) {
        m_HPWaterCausticValid = false;
        m_HPWaterCausticFilteredValid = false;
        m_HPWaterCausticComputeIrradianceValid = false;
        m_HPWaterCausticComputeIrradianceRan = false;
        m_HPWaterCausticFilterIterations = 0;
        m_HPWaterCausticAtlasConsumed = false;
        return false;
    }

    if (strength <= 0.0001f || lightIntensity <= 0.0001f) {
        m_HPWaterCausticValid = false;
        m_HPWaterCausticFilteredValid = false;
        m_HPWaterCausticComputeIrradianceValid = false;
        m_HPWaterCausticComputeIrradianceRan = false;
        m_HPWaterCausticFilterIterations = 0;
        m_HPWaterCausticAtlasConsumed = false;
        return false;
    }

    const bool computeIrradianceValid =
        RunHPWaterCausticComputeIrradiance(nearClip, farClip, lightDir, lightIntensity, strength, scale, depthFade);

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
    m_HPWaterCausticShader->SetInt("u_CausticRGBDispersion", rgbDispersion ? 1 : 0);
    m_HPWaterCausticShader->SetFloat("u_CausticDispersionStrength",
        std::clamp(dispersionStrength, 0.0f, 2.0f));
    m_HPWaterCausticShader->SetInt("u_HPWaterMaskEnabled", m_HPWaterMaskValid ? 1 : 0);
    m_HPWaterCausticShader->SetInt("u_HPWaterCausticAtlasEnabled", atlasValid ? 1 : 0);
    m_HPWaterCausticShader->SetInt("u_HPWaterCausticComputeEnabled", computeIrradianceValid ? 1 : 0);
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
    return true;
}

bool DeferredRenderer::RunHPWaterCausticComputeIrradiance(float nearClip,
                                                          float farClip,
                                                          const glm::vec3& lightDir,
                                                          float lightIntensity,
                                                          float strength,
                                                          float scale,
                                                          float depthFade) {
    m_HPWaterCausticComputeIrradianceRan = false;
    m_HPWaterCausticComputeIrradianceValid = false;

    if (!m_HPWaterCausticComputeShader || m_HPWaterCausticComputeIrradianceTexture == 0 ||
        !m_GBuffer || !m_HPWaterGBuffer || m_Width == 0 || m_Height == 0) {
        return false;
    }

    const bool atlasValid = m_HPWaterCausticAtlasValid && m_HPWaterCausticAtlasFBO &&
        m_HPWaterCausticAtlasFBO->GetColorAttachmentID() != 0 &&
        m_HPWaterCausticAtlasFBO->GetDepthAttachmentID() != 0;

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

    glBindImageTexture(0,
                       static_cast<GLuint>(m_HPWaterCausticComputeIrradianceTexture),
                       0,
                       GL_FALSE,
                       0,
                       GL_WRITE_ONLY,
                       GL_R16F);

    m_HPWaterCausticComputeShader->SetInt("u_OutputWidth", static_cast<int>(m_Width));
    m_HPWaterCausticComputeShader->SetInt("u_OutputHeight", static_cast<int>(m_Height));
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
    m_HPWaterCausticComputeShader->SetFloat("u_CausticStrength", std::clamp(strength, 0.0f, 8.0f));
    m_HPWaterCausticComputeShader->SetFloat("u_CausticScale", std::clamp(scale, 0.1f, 128.0f));
    m_HPWaterCausticComputeShader->SetFloat("u_CausticDepthFade", std::clamp(depthFade, 0.1f, 500.0f));

    m_HPWaterCausticComputeShader->Dispatch((m_Width + 15u) / 16u, (m_Height + 15u) / 16u, 1u);
    m_HPWaterCausticComputeShader->MemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    m_HPWaterCausticComputeShader->Unbind();

    m_HPWaterCausticComputeIrradianceRan = true;
    m_HPWaterCausticComputeIrradianceValid = true;
    return true;
}

bool DeferredRenderer::RunHPWaterCausticFilterPass(const std::shared_ptr<Framebuffer>& inputFBO,
                                                   const std::shared_ptr<Framebuffer>& outputFBO,
                                                   float stride,
                                                   float radius,
                                                   float depthSigma) {
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
    m_HPWaterCausticFilterShader->SetInt("u_HPWaterMaskEnabled", m_HPWaterMaskValid ? 1 : 0);

    glBindVertexArray(m_QuadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);

    outputFBO->Unbind();
    return true;
}

bool DeferredRenderer::FilterHPWaterCaustics(float radius,
                                             float depthSigma,
                                             int iterations) {
    if (!m_HPWaterCausticFilterShader || !m_HPWaterCausticFBO ||
        !m_HPWaterCausticFilteredFBO || !m_HPWaterCausticFilterScratchFBO ||
        !m_HPWaterCausticValid || m_QuadVAO == 0) {
        m_HPWaterCausticFilteredValid = false;
        m_HPWaterCausticFilterIterations = 0;
        return false;
    }

    m_HPWaterCausticFilterIterations = 0;
    const int clampedIterations = std::clamp(iterations, 1, 2);
    std::shared_ptr<Framebuffer> inputFBO = m_HPWaterCausticFBO;
    for (int i = 0; i < clampedIterations; ++i) {
        const bool lastPass = i == clampedIterations - 1;
        const auto outputFBO = lastPass ? m_HPWaterCausticFilteredFBO : m_HPWaterCausticFilterScratchFBO;
        const float stride = static_cast<float>(1u << static_cast<uint32_t>(i));
        if (!RunHPWaterCausticFilterPass(inputFBO, outputFBO, stride, radius, depthSigma)) {
            m_HPWaterCausticFilteredValid = false;
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
        return false;
    }

    resolution = std::clamp(resolution, 16u, 1024u);
    CreateHPWaterFluidFBO(resolution);
    if (!m_HPWaterFluidCurrentFBO || !m_HPWaterFluidPreviousFBO || !m_HPWaterFluidNextFBO) {
        m_HPWaterFluidValid = false;
        m_HPWaterFluidComputeRan = false;
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
        m_HPWaterFluidComputeShader->SetFloat("u_WaveSpeed", std::clamp(waveSpeed, 0.0f, 2.0f));
        m_HPWaterFluidComputeShader->SetFloat("u_DampingFactor", std::clamp(damping, 0.0f, 0.98f));
        m_HPWaterFluidComputeShader->SetFloat("u_WaveSourceIntensity", std::clamp(sourceIntensity, -1.0f, 1.0f));
        m_HPWaterFluidComputeShader->SetFloat("u_WaveSourceRadius", std::max(sourceRadiusPixels, 1.0f));
        m_HPWaterFluidComputeShader->SetVec3("u_WaveSourceUV", glm::vec3(sourceU, sourceV, 0.0f));
        m_HPWaterFluidComputeShader->SetInt("u_ObstacleMaskEnabled", m_HPWaterFluidObstacleValid ? 1 : 0);
        m_HPWaterFluidComputeShader->SetInt("u_HeightFieldEnabled", heightFieldValid ? 1 : 0);
        m_HPWaterFluidComputeShader->SetFloat("u_HeightObstacleEpsilon", 0.0005f);

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

    m_HPWaterFluidShader->SetFloat("u_WaveSpeed", std::clamp(waveSpeed, 0.0f, 2.0f));
    m_HPWaterFluidShader->SetFloat("u_DampingFactor", std::clamp(damping, 0.0f, 0.98f));
    m_HPWaterFluidShader->SetFloat("u_WaveSourceIntensity", std::clamp(sourceIntensity, -1.0f, 1.0f));
    m_HPWaterFluidShader->SetFloat("u_WaveSourceRadius", std::max(sourceRadiusPixels, 1.0f));
    m_HPWaterFluidShader->SetVec3("u_WaveSourceUV", glm::vec3(sourceU, sourceV, 0.0f));
    m_HPWaterFluidShader->SetInt("u_ObstacleMaskEnabled", m_HPWaterFluidObstacleValid ? 1 : 0);
    m_HPWaterFluidShader->SetInt("u_HeightFieldEnabled", heightFieldValid ? 1 : 0);
    m_HPWaterFluidShader->SetFloat("u_HeightObstacleEpsilon", 0.0005f);

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

uint32_t DeferredRenderer::GetHPWaterMaskTexture() const {
    if (!m_HPWaterMaskFBO) return 0;
    return static_cast<uint32_t>(m_HPWaterMaskFBO->GetColorAttachmentID());
}

uint32_t DeferredRenderer::GetHPWaterVolumeTexture(int index) const {
    if (!m_HPWaterVolumeFBO) return 0;
    return static_cast<uint32_t>(m_HPWaterVolumeFBO->GetColorAttachmentID(index));
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
