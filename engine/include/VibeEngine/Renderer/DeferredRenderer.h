/*
 * DeferredRenderer — Manages the G-buffer and deferred lighting pass.
 *
 * The deferred rendering pipeline splits rendering into two phases:
 *   1. Geometry Pass: Render all opaque objects into the G-buffer (MRT)
 *   2. Lighting Pass: Fullscreen quad reads G-buffer, computes PBR lighting
 *
 * Transparent objects must still be rendered with the forward pipeline
 * after the deferred lighting pass.
 *
 * G-Buffer layout:
 *   RT0 (RGBA16F): WorldPosition.xyz + Metallic
 *   RT1 (RGBA16F): Normal.xyz (encoded [0,1]) + Roughness
 *   RT2 (RGBA8):   Albedo.rgb + AO
 *   RT3 (RGBA8):   Emission.rgb + Flags
 */
#pragma once

#include "VibeEngine/Renderer/Framebuffer.h"
#include "VibeEngine/Renderer/Shader.h"
#include <glm/glm.hpp>
#include <memory>
#include <cstdint>

namespace VE {

/// Debug view modes for visualizing individual G-buffer channels.
enum class GBufferDebugView {
    None = 0,     // Normal lit output
    Position,
    Normals,
    Albedo,
    Metallic,
    Roughness,
    AO,
    Emission,
    Depth
};

class DeferredRenderer {
public:
    DeferredRenderer() = default;
    ~DeferredRenderer() = default;

    /// Initialize the G-buffer FBO and load deferred shaders.
    void Init(uint32_t width, uint32_t height);

    /// Shutdown and release GPU resources.
    void Shutdown();

    /// Recreate G-buffer when viewport size changes.
    void Resize(uint32_t width, uint32_t height);

    /// Begin geometry pass: bind G-buffer FBO, clear all attachments.
    void BeginGeometryPass();

    /// End geometry pass: unbind G-buffer FBO.
    void EndGeometryPass();

    /// Get the G-buffer shader for rendering opaque geometry.
    std::shared_ptr<Shader> GetGBufferShader() const { return m_GBufferShader; }

    /// Execute the deferred lighting pass (fullscreen quad).
    /// Reads G-buffer textures and computes PBR lighting.
    void LightingPass();

    /// Get the lit output texture ID for post-processing.
    uint32_t GetOutputTexture() const;

    /// Get G-buffer depth texture (for SSAO, post-processing, etc.)
    uint32_t GetDepthTexture() const;

    /// Get the output framebuffer width/height.
    uint32_t GetWidth() const { return m_Width; }
    uint32_t GetHeight() const { return m_Height; }

    /// Bind G-buffer textures to specified texture units for the lighting shader.
    void BindGBufferTextures(int startUnit = 0);

    /// Get the lighting shader (for setting uniforms externally).
    std::shared_ptr<Shader> GetLightingShader() const { return m_LightingShader; }

    /// Debug: render a specific G-buffer channel to the output.
    void DebugVisualize(GBufferDebugView view);

    /// Whether the renderer is initialized.
    bool IsInitialized() const { return m_Initialized; }

    /// Copy depth from G-buffer to the main framebuffer (for forward transparent pass).
    void BlitDepthTo(uint32_t targetFBO, uint32_t width, uint32_t height);

private:
    void CreateLightingFBO();

    uint32_t m_Width = 0;
    uint32_t m_Height = 0;
    bool m_Initialized = false;

    // G-Buffer (MRT: 4 color attachments + depth)
    std::shared_ptr<Framebuffer> m_GBuffer;

    // Lighting output FBO (single RGBA16F attachment)
    std::shared_ptr<Framebuffer> m_LightingFBO;

    // Shaders
    std::shared_ptr<Shader> m_GBufferShader;
    std::shared_ptr<Shader> m_LightingShader;
    std::shared_ptr<Shader> m_DebugShader;

    // Dummy VAO for fullscreen triangle draws
    uint32_t m_QuadVAO = 0;
};

} // namespace VE
