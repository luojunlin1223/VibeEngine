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
uniform int u_DebugMode; // 1=Pos, 2=Normal, 3=Albedo, 4=Metallic, 5=Roughness, 6=AO, 7=Emission, 8=Depth

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

    // ── Create lighting output FBO ──
    CreateLightingFBO();

    // ── Load G-Buffer shader ──
    m_GBufferShader = Shader::CreateFromFile("shaders/GBuffer.shader");
    if (!m_GBufferShader) {
        VE_ENGINE_ERROR("DeferredRenderer: Failed to load GBuffer.shader");
    }

    // ── Load Deferred Lighting shader ──
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
    m_GBufferShader.reset();
    m_LightingShader.reset();
    m_DebugShader.reset();

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

void DeferredRenderer::Resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0 || width > 8192 || height > 8192) return;
    if (width == m_Width && height == m_Height) return;

    m_Width = width;
    m_Height = height;

    if (m_GBuffer)
        m_GBuffer->Resize(width, height);

    if (m_LightingFBO)
        m_LightingFBO->Resize(width, height);
}

void DeferredRenderer::BeginGeometryPass() {
    if (!m_GBuffer) return;

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

void DeferredRenderer::BindGBufferTextures(int startUnit) {
    if (!m_GBuffer) return;

    for (int i = 0; i < m_GBuffer->GetColorAttachmentCount(); ++i) {
        glActiveTexture(GL_TEXTURE0 + startUnit + i);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_GBuffer->GetColorAttachmentID(i)));
    }
}

void DeferredRenderer::LightingPass() {
    if (!m_LightingShader || !m_LightingFBO || !m_GBuffer) return;

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

uint32_t DeferredRenderer::GetOutputTexture() const {
    if (!m_LightingFBO) return 0;
    return static_cast<uint32_t>(m_LightingFBO->GetColorAttachmentID());
}

uint32_t DeferredRenderer::GetDepthTexture() const {
    if (!m_GBuffer) return 0;
    return static_cast<uint32_t>(m_GBuffer->GetDepthAttachmentID());
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
