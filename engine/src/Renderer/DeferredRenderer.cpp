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

    // ── Create lighting output FBO ──
    CreateLightingFBO();
    CreateHPWaterCompositeFBO();
    CreateHPWaterVolumeFBO();

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
    m_GBuffer.reset();
    m_LightingFBO.reset();
    m_HPWaterGBuffer.reset();
    m_HPWaterCompositeFBO.reset();
    m_HPWaterVolumeFBO.reset();
    m_HPWaterVolumeTemporalFBO.reset();
    m_HPWaterVolumeHistoryFBO.reset();
    m_HPWaterVolumeFilteredFBO.reset();
    m_HPWaterVolumeFilterScratchFBO.reset();
    m_GBufferShader.reset();
    m_HPWaterGBufferShader.reset();
    m_HPWaterCompositeShader.reset();
    m_HPWaterVolumeShader.reset();
    m_HPWaterVolumeTemporalShader.reset();
    m_HPWaterVolumeFilterShader.reset();
    m_LightingShader.reset();
    m_DebugShader.reset();
    m_HPWaterCompositeValid = false;
    m_HPWaterVolumeValid = false;
    m_HPWaterVolumeTemporalValid = false;
    m_HPWaterVolumeHistoryValid = false;
    m_HPWaterVolumeFilteredValid = false;
    m_HPWaterVolumeFilterIterations = 0;

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
    m_HPWaterVolumeValid = false;
    m_HPWaterVolumeTemporalValid = false;
    m_HPWaterVolumeHistoryValid = false;
    m_HPWaterVolumeFilteredValid = false;
    m_HPWaterVolumeFilterIterations = 0;
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

    m_HPWaterCompositeValid = false;
    m_HPWaterVolumeValid = false;
    m_HPWaterVolumeTemporalValid = false;
    m_HPWaterVolumeHistoryValid = false;
    m_HPWaterVolumeFilteredValid = false;
    m_HPWaterVolumeFilterIterations = 0;
}

void DeferredRenderer::ClearHPWaterGBuffer() {
    if (!m_HPWaterGBuffer) return;

    m_HPWaterGBuffer->Bind();
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
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
    m_HPWaterVolumeValid = false;
    m_HPWaterVolumeFilteredValid = false;
    m_HPWaterVolumeFilterIterations = 0;

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

bool DeferredRenderer::CompositeHPWater(float nearClip,
                                        float farClip,
                                        float refractionStrength,
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

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_LightingFBO->GetColorAttachmentID()));
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

    auto volumeForComposite = m_HPWaterVolumeFilteredValid && m_HPWaterVolumeFilteredFBO
        ? m_HPWaterVolumeFilteredFBO
        : m_HPWaterVolumeFBO;

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

    m_HPWaterCompositeShader->SetFloat("u_NearClip", nearClip);
    m_HPWaterCompositeShader->SetFloat("u_FarClip", farClip);
    m_HPWaterCompositeShader->SetFloat("u_RefractionStrength", refractionStrength);
    m_HPWaterCompositeShader->SetMat4("u_InverseViewProjection", inverseViewProjection);
    m_HPWaterCompositeShader->SetInt("u_HPWaterVolumeEnabled", (m_HPWaterVolumeValid || m_HPWaterVolumeFilteredValid) ? 1 : 0);

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
                                               const glm::mat4& inverseViewProjection) {
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

    m_HPWaterVolumeShader->SetFloat("u_NearClip", nearClip);
    m_HPWaterVolumeShader->SetFloat("u_FarClip", farClip);
    m_HPWaterVolumeShader->SetVec3("u_LightDir", lightDir);
    m_HPWaterVolumeShader->SetVec3("u_LightColor", lightColor);
    m_HPWaterVolumeShader->SetFloat("u_LightIntensity", lightIntensity);
    m_HPWaterVolumeShader->SetVec3("u_CameraPosition", cameraPosition);
    m_HPWaterVolumeShader->SetMat4("u_InverseViewProjection", inverseViewProjection);

    glBindVertexArray(m_QuadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);

    m_HPWaterVolumeFBO->Unbind();
    m_HPWaterVolumeValid = true;
    m_HPWaterVolumeTemporalValid = false;
    m_HPWaterVolumeFilteredValid = false;
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
    return m_HPWaterVolumeFilteredValid;
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
