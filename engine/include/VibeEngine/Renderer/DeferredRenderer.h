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
 * HPWater parity note:
 * HPWater uses a separate water G-buffer in the reference Unity pipeline. Keep
 * that surface data separate from the generic opaque G-buffer so refraction,
 * volumetric water lighting, caustics, and fluid simulation passes can evolve
 * without overloading the material flags attachment.
 *
 * G-Buffer layout:
 *   RT0 (RGBA16F): WorldPosition.xyz + Metallic
 *   RT1 (RGBA16F): Normal.xyz (encoded [0,1]) + Roughness
 *   RT2 (RGBA8):   Albedo.rgb + AO
 *   RT3 (RGBA8):   Emission.rgb + Flags
 *
 * HPWater G-Buffer layout:
 *   RT0 (RGBA16F): Water normal.xyz encoded to [0,1] + roughness
 *   RT1 (RGBA16F): Scatter color.rgb + thickness
 *   RT2 (RGBA16F): Absorption color.rgb + foam
 *
 * HPWater composite:
 *   Reads the opaque scene color/depth plus HPWater surface data/depth and
 *   writes a resolved water composite texture. This mirrors the reference
 *   HPWater split where refraction consumes the water G-buffer instead of
 *   shading water as generic opaque geometry.
 *
 * HPWater refraction data:
 *   RT1 of the composite FBO stores refractedWorldPos.xyz + rayLength.
 *   RT2 stores refractUV.xy + refractedDepth + normalized water thickness for
 *   diagnostics. Refraction marches against a dedicated opaque scene-depth
 *   pyramid so the water pass can evolve toward HPWater's Hi-Z path without
 *   modifying the generic deferred depth buffer.
 *
 * HPWater volume data:
 *   Half-resolution MRT targets store volumetric in-scattering, transmittance,
 *   and refracted linear depth. A full-resolution joint bilateral upsample
 *   resolves the filtered low-res buffers before composite. The dataflow now mirrors the reference HPWater volume path:
 *   low-res accumulation -> temporal reprojection/history -> a-trous spatial
 *   filtering -> full-res upsample -> full-res composite.
 *
 * HPWater mask:
 *   HPWater's Unity path relies on stencil to isolate water pixels in later
 *   fullscreen passes. VibeEngine currently uses an explicit R8 water mask
 *   texture generated from the dedicated HPWater depth buffer as the equivalent
 *   GPU-side pass-isolation resource.
 *
 * HPWater light-space caustic atlas:
 *   Captures water-only surface payloads from each directional-light cascade
 *   into a 2x2 atlas. This is the first resource needed to replace the current
 *   screen-space caustic approximation with HPWater-style light-space caustic
 *   accumulation.
 */
#pragma once

#include "VibeEngine/Renderer/Framebuffer.h"
#include "VibeEngine/Renderer/Shader.h"
#include "VibeEngine/Renderer/ComputeShader.h"
#include <glm/glm.hpp>
#include <memory>
#include <cstdint>
#include <vector>

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
    Depth,
    HPWaterNormalRoughness,
    HPWaterScatterThickness,
    HPWaterAbsorptionFoam
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

    /// Begin HPWater surface-data pass.
    void BeginHPWaterGBufferPass();

    /// End HPWater surface-data pass.
    void EndHPWaterGBufferPass();

    /// Get the G-buffer shader for rendering opaque geometry.
    std::shared_ptr<Shader> GetGBufferShader() const { return m_GBufferShader; }

    /// Get the HPWater G-buffer shader for rendering water surface payloads.
    std::shared_ptr<Shader> GetHPWaterGBufferShader() const { return m_HPWaterGBufferShader; }

    /// Get the HPWater light-space caustic atlas shader.
    std::shared_ptr<Shader> GetHPWaterCausticAtlasShader() const { return m_HPWaterCausticAtlasShader; }

    /// Get the HPWater top-down fluid height capture shader.
    std::shared_ptr<Shader> GetHPWaterFluidHeightCaptureShader() const { return m_HPWaterFluidHeightCaptureShader; }

    /// Execute the deferred lighting pass (fullscreen quad).
    /// Reads G-buffer textures and computes PBR lighting.
    void LightingPass();

    /// Composite HPWater over the lit scene using dedicated water G-buffer data.
    bool CompositeHPWater(float nearClip,
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
                          const glm::mat4& inverseViewProjection);

    /// Build the opaque scene-depth pyramid used by HPWater refraction.
    bool BuildHPWaterDepthPyramid();

    /// Build the explicit full-resolution HPWater coverage mask.
    bool BuildHPWaterMask();

    /// Accumulate low-resolution HPWater volume lighting from refraction data.
    bool AccumulateHPWaterVolume(float nearClip,
                                 float farClip,
                                 const glm::vec3& lightDir,
                                 const glm::vec3& lightColor,
                                 float lightIntensity,
                                 const glm::vec3& cameraPosition,
                                 const glm::mat4& inverseViewProjection,
                                 float macroScatterStrength,
                                 float causticVolumeStrength);

    /// Reproject and blend low-resolution HPWater volume data with previous frame history.
    bool TemporalFilterHPWaterVolume(const glm::mat4& currentViewProjection,
                                     const glm::mat4& previousViewProjection);

    /// Run multi-iteration depth-aware a-trous filtering over low-resolution HPWater volume buffers.
    bool FilterHPWaterVolume();

    /// Resolve filtered low-resolution HPWater volume buffers into full-resolution textures.
    bool UpsampleHPWaterVolume(float nearClip, float farClip);

    /// Accumulate first-pass HPWater caustic energy into a full-resolution texture.
    bool AccumulateHPWaterCaustics(float nearClip,
                                   float farClip,
                                   const glm::vec3& lightDir,
                                   const glm::vec3& lightColor,
                                   float lightIntensity,
                                   float strength,
                                   float scale,
                                   float depthFade,
                                   bool rgbDispersion,
                                   float dispersionStrength);

    /// Denoise/filter HPWater caustic energy before composite and volume lighting.
    bool FilterHPWaterCaustics(float radius,
                               float depthSigma,
                               int iterations);

    /// Step HPWater's GPU fluid height field. This is the OpenGL ping-pong
    /// equivalent of HPWater's compute wave equation texture path.
    bool UpdateHPWaterFluidDynamics(uint32_t resolution,
                                    float waveSpeed,
                                    float damping,
                                    float sourceU,
                                    float sourceV,
                                    float sourceIntensity,
                                    float sourceRadiusPixels,
                                    const glm::vec3& boxCenter,
                                    const glm::vec3& boxSize);

    /// Upload the top-down scene obstacle mask consumed by HPWater fluid dynamics.
    /// Pixels are R8, 0 = free water and 255 = solid scene obstacle.
    bool UploadHPWaterFluidObstacleMask(uint32_t resolution,
                                        const std::vector<uint8_t>& maskPixels,
                                        const glm::vec3& boxCenter,
                                        const glm::vec3& boxSize);

    /// Upload HPWater-style top-down height textures consumed by fluid dynamics.
    /// Values are normalized into the water simulation volume: water < scene means obstacle.
    bool UploadHPWaterFluidHeightFields(uint32_t resolution,
                                        const std::vector<float>& waterHeights,
                                        const std::vector<float>& sceneHeights,
                                        const glm::vec3& boxCenter,
                                        const glm::vec3& boxSize);

    bool BeginHPWaterFluidWaterHeightCapture(uint32_t resolution,
                                             const glm::vec3& boxCenter,
                                             const glm::vec3& boxSize);
    bool BeginHPWaterFluidSceneHeightCapture(uint32_t resolution,
                                             const glm::vec3& boxCenter,
                                             const glm::vec3& boxSize);
    void EndHPWaterFluidWaterHeightCapture(bool valid);
    void EndHPWaterFluidSceneHeightCapture(bool valid);

    /// Get the lit output texture ID for post-processing.
    uint32_t GetOutputTexture() const;

    /// Get the raw deferred lighting texture before HPWater composite.
    uint32_t GetLightingTexture() const;

    /// Get the HPWater composite texture.
    uint32_t GetHPWaterCompositeTexture() const;

    /// Get the HPWater precomputed refraction payload texture.
    uint32_t GetHPWaterRefractionDataTexture() const;

    /// Get the HPWater refraction metadata texture.
    uint32_t GetHPWaterRefractionMetaTexture() const;

    /// Get HPWater opaque scene-depth pyramid texture.
    uint32_t GetHPWaterDepthPyramidTexture() const { return m_HPWaterDepthPyramidTexture; }
    uint32_t GetHPWaterDepthPyramidMipCount() const { return m_HPWaterDepthPyramidMipCount; }
    bool IsHPWaterDepthPyramidValid() const { return m_HPWaterDepthPyramidValid; }
    uint32_t GetHPWaterSceneColorMipCount() const { return m_HPWaterSceneColorMipCount; }
    bool IsHPWaterSceneColorMipValid() const { return m_HPWaterSceneColorMipValid; }
    uint32_t GetHPWaterFGDLUTTexture() const { return m_HPWaterFGDLUTTexture; }
    uint32_t GetHPWaterFGDLUTResolution() const { return m_HPWaterFGDLUTResolution; }
    bool IsHPWaterFGDLUTValid() const { return m_HPWaterFGDLUTValid; }

    /// Get HPWater explicit coverage mask texture.
    uint32_t GetHPWaterMaskTexture() const;
    bool IsHPWaterMaskValid() const { return m_HPWaterMaskValid; }

    /// Get HPWater low-resolution volume texture ID by index.
    uint32_t GetHPWaterVolumeTexture(int index) const;

    /// Whether the current HPWater volume accumulation is valid.
    bool IsHPWaterVolumeValid() const { return m_HPWaterVolumeValid; }

    /// Whether the filtered HPWater volume buffers are valid for composite.
    bool IsHPWaterVolumeFilteredValid() const { return m_HPWaterVolumeFilteredValid; }
    uint32_t GetHPWaterVolumeFilterIterations() const { return m_HPWaterVolumeFilterIterations; }
    bool IsHPWaterVolumeTemporalValid() const { return m_HPWaterVolumeTemporalValid; }
    bool HasHPWaterVolumeHistory() const { return m_HPWaterVolumeHistoryValid; }
    bool IsHPWaterVolumeUpsampledValid() const { return m_HPWaterVolumeUpsampledValid; }
    void InvalidateHPWaterVolumeHistory();

    /// Whether the current output texture is the HPWater composite.
    bool IsHPWaterCompositeValid() const { return m_HPWaterCompositeValid; }

    /// Get G-buffer depth texture (for SSAO, post-processing, etc.)
    uint32_t GetDepthTexture() const;

    /// Get HPWater surface data texture ID by index.
    uint32_t GetHPWaterGBufferTexture(int index) const;

    /// Get HPWater depth texture.
    uint32_t GetHPWaterDepthTexture() const;

    /// Number of HPWater G-buffer color attachments.
    int GetHPWaterGBufferAttachmentCount() const;

    /// Whether the dedicated HPWater G-buffer exists.
    bool HasHPWaterGBuffer() const { return m_HPWaterGBuffer != nullptr; }

    /// Get the output framebuffer width/height.
    uint32_t GetWidth() const { return m_Width; }
    uint32_t GetHeight() const { return m_Height; }
    uint32_t GetHPWaterVolumeWidth() const;
    uint32_t GetHPWaterVolumeHeight() const;
    uint32_t GetHPWaterVolumeFilteredTexture(int index) const;
    uint32_t GetHPWaterVolumeHistoryTexture(int index) const;
    uint32_t GetHPWaterVolumeUpsampledTexture(int index) const;
    uint32_t GetHPWaterCausticTexture() const;
    bool IsHPWaterCausticValid() const { return m_HPWaterCausticValid; }
    uint32_t GetHPWaterCausticComputeIrradianceTexture() const { return m_HPWaterCausticComputeIrradianceTexture; }
    bool IsHPWaterCausticComputeIrradianceValid() const { return m_HPWaterCausticComputeIrradianceValid; }
    bool DidRunHPWaterCausticComputeIrradiance() const { return m_HPWaterCausticComputeIrradianceRan; }
    uint32_t GetHPWaterCausticFilteredTexture() const;
    bool IsHPWaterCausticFilteredValid() const { return m_HPWaterCausticFilteredValid; }
    uint32_t GetHPWaterCausticFilterIterations() const { return m_HPWaterCausticFilterIterations; }
    bool BeginHPWaterCausticAtlas(uint32_t tileResolution);
    void BeginHPWaterCausticAtlasCascade(uint32_t cascadeIndex);
    void EndHPWaterCausticAtlas(bool valid);
    uint32_t GetHPWaterCausticAtlasTexture() const;
    uint32_t GetHPWaterCausticAtlasDepthTexture() const;
    bool IsHPWaterCausticAtlasValid() const { return m_HPWaterCausticAtlasValid; }
    bool IsHPWaterCausticAtlasConsumed() const { return m_HPWaterCausticAtlasConsumed; }
    uint32_t GetHPWaterCausticAtlasTileResolution() const { return m_HPWaterCausticAtlasTileResolution; }
    uint32_t GetHPWaterCausticAtlasWidth() const;
    uint32_t GetHPWaterCausticAtlasHeight() const;
    uint32_t GetHPWaterCausticAtlasCascadeCount() const { return 4; }

    uint32_t GetHPWaterFluidHeightTexture() const;
    uint32_t GetHPWaterFluidObstacleTexture() const { return m_HPWaterFluidObstacleTexture; }
    uint32_t GetHPWaterFluidWaterHeightTexture() const;
    uint32_t GetHPWaterFluidSceneHeightTexture() const;
    uint32_t GetHPWaterFluidResolution() const { return m_HPWaterFluidResolution; }
    bool IsHPWaterFluidDynamicsValid() const { return m_HPWaterFluidValid; }
    bool DidHPWaterFluidComputeRun() const { return m_HPWaterFluidComputeRan; }
    bool IsHPWaterFluidObstacleValid() const { return m_HPWaterFluidObstacleValid; }
    bool IsHPWaterFluidHeightFieldValid() const { return m_HPWaterFluidHeightFieldValid; }
    bool DidHPWaterFluidHeightCaptureRun() const { return m_HPWaterFluidHeightCaptureRan; }
    bool IsHPWaterFluidHeightCaptureValid() const { return m_HPWaterFluidHeightCaptureValid; }
    glm::vec3 GetHPWaterFluidBoxCenter() const { return m_HPWaterFluidBoxCenter; }
    glm::vec3 GetHPWaterFluidBoxSize() const { return m_HPWaterFluidBoxSize; }

    /// Bind G-buffer textures to specified texture units for the lighting shader.
    void BindGBufferTextures(int startUnit = 0);

    /// Bind HPWater G-buffer textures to specified texture units.
    void BindHPWaterGBufferTextures(int startUnit = 8);

    /// Get the lighting shader (for setting uniforms externally).
    std::shared_ptr<Shader> GetLightingShader() const { return m_LightingShader; }

    /// Debug: render a specific G-buffer channel to the output.
    void DebugVisualize(GBufferDebugView view);

    /// Whether the renderer is initialized.
    bool IsInitialized() const { return m_Initialized; }

    /// Copy depth from G-buffer to the main framebuffer (for forward transparent pass).
    void BlitDepthTo(uint32_t targetFBO, uint32_t width, uint32_t height);

    /// Bind the deferred lighting target for forward transparent rendering.
    /// Copies G-buffer depth first so transparent objects test against opaque geometry.
    void BeginForwardPass();

    /// Finish forward transparent rendering into the lighting target.
    void EndForwardPass();

private:
    void CreateLightingFBO();
    void CreateHPWaterCompositeFBO();
    void CreateHPWaterVolumeFBO();
    void CreateHPWaterCausticFBO();
    void CreateHPWaterCausticComputeTexture();
    void CreateHPWaterFGDLUT();
    void CreateHPWaterCausticAtlasFBO(uint32_t tileResolution);
    void CreateHPWaterGBuffer();
    void CreateHPWaterMaskFBO();
    void CreateHPWaterDepthPyramid();
    void CreateHPWaterFluidFBO(uint32_t resolution);
    void CreateHPWaterFluidHeightCaptureFBO(uint32_t resolution);
    void DestroyHPWaterFluidObstacleTexture();
    void DestroyHPWaterFluidHeightFieldTextures();
    bool BeginHPWaterFluidHeightCaptureTarget(const std::shared_ptr<Framebuffer>& target,
                                              uint32_t resolution,
                                              const glm::vec3& boxCenter,
                                              const glm::vec3& boxSize);
    void EndHPWaterFluidHeightCaptureTarget();
    void DestroyHPWaterDepthPyramid();
    void DestroyHPWaterCausticComputeTexture();
    void DestroyHPWaterFGDLUT();
    void ClearHPWaterFluidFBOs();
    void ClearHPWaterGBuffer();
    void CommitHPWaterVolumeHistory();
    bool RunHPWaterVolumeFilterPass(const std::shared_ptr<Framebuffer>& inputFBO,
                                    const std::shared_ptr<Framebuffer>& outputFBO,
                                    float stride);
    bool RunHPWaterCausticFilterPass(const std::shared_ptr<Framebuffer>& inputFBO,
                                     const std::shared_ptr<Framebuffer>& outputFBO,
                                     float stride,
                                     float radius,
                                     float depthSigma);
    bool RunHPWaterCausticComputeIrradiance(float nearClip,
                                            float farClip,
                                            const glm::vec3& lightDir,
                                            float lightIntensity,
                                            float strength,
                                            float scale,
                                            float depthFade);
    static uint32_t GetHalfResolution(uint32_t value);

    uint32_t m_Width = 0;
    uint32_t m_Height = 0;
    bool m_Initialized = false;

    // G-Buffer (MRT: 4 color attachments + depth)
    std::shared_ptr<Framebuffer> m_GBuffer;

    // Lighting output FBO (single RGBA16F attachment)
    std::shared_ptr<Framebuffer> m_LightingFBO;

    // Dedicated HPWater surface payloads (separate from generic opaque G-buffer).
    std::shared_ptr<Framebuffer> m_HPWaterGBuffer;

    // Explicit full-resolution HPWater coverage mask.
    std::shared_ptr<Framebuffer> m_HPWaterMaskFBO;
    bool m_HPWaterMaskValid = false;

    // HPWater final composite output and precomputed refraction payload.
    std::shared_ptr<Framebuffer> m_HPWaterCompositeFBO;
    bool m_HPWaterCompositeValid = false;
    uint32_t m_HPWaterSceneColorMipCount = 1;
    bool m_HPWaterSceneColorMipValid = false;
    uint32_t m_HPWaterFGDLUTTexture = 0;
    uint32_t m_HPWaterFGDLUTResolution = 128;
    bool m_HPWaterFGDLUTValid = false;

    // HPWater opaque scene-depth pyramid for Hi-Z assisted refraction.
    uint32_t m_HPWaterDepthPyramidTexture = 0;
    uint32_t m_HPWaterDepthPyramidFBO = 0;
    uint32_t m_HPWaterDepthPyramidMipCount = 0;
    bool m_HPWaterDepthPyramidValid = false;

    // Low-resolution HPWater volume accumulation targets.
    std::shared_ptr<Framebuffer> m_HPWaterVolumeFBO;
    std::shared_ptr<Framebuffer> m_HPWaterVolumeTemporalFBO;
    std::shared_ptr<Framebuffer> m_HPWaterVolumeHistoryFBO;
    std::shared_ptr<Framebuffer> m_HPWaterVolumeFilteredFBO;
    std::shared_ptr<Framebuffer> m_HPWaterVolumeFilterScratchFBO;
    std::shared_ptr<Framebuffer> m_HPWaterVolumeUpsampledFBO;
    bool m_HPWaterVolumeValid = false;
    bool m_HPWaterVolumeTemporalValid = false;
    bool m_HPWaterVolumeHistoryValid = false;
    bool m_HPWaterVolumeFilteredValid = false;
    bool m_HPWaterVolumeUpsampledValid = false;
    uint32_t m_HPWaterVolumeFilterIterations = 0;

    // Full-resolution caustic energy consumed by the HPWater composite pass.
    std::shared_ptr<Framebuffer> m_HPWaterCausticFBO;
    std::shared_ptr<Framebuffer> m_HPWaterCausticFilteredFBO;
    std::shared_ptr<Framebuffer> m_HPWaterCausticFilterScratchFBO;
    bool m_HPWaterCausticValid = false;
    bool m_HPWaterCausticFilteredValid = false;
    uint32_t m_HPWaterCausticFilterIterations = 0;
    uint32_t m_HPWaterCausticComputeIrradianceTexture = 0;
    bool m_HPWaterCausticComputeIrradianceValid = false;
    bool m_HPWaterCausticComputeIrradianceRan = false;

    // Water-only light-space cascade atlas for HPWater-style caustic accumulation.
    std::shared_ptr<Framebuffer> m_HPWaterCausticAtlasFBO;
    bool m_HPWaterCausticAtlasValid = false;
    bool m_HPWaterCausticAtlasConsumed = false;
    uint32_t m_HPWaterCausticAtlasTileResolution = 0;

    // HPWater FluidDynamics wave height ping-pong textures.
    std::shared_ptr<Framebuffer> m_HPWaterFluidCurrentFBO;
    std::shared_ptr<Framebuffer> m_HPWaterFluidPreviousFBO;
    std::shared_ptr<Framebuffer> m_HPWaterFluidNextFBO;
    std::shared_ptr<Framebuffer> m_HPWaterFluidWaterHeightFBO;
    std::shared_ptr<Framebuffer> m_HPWaterFluidSceneHeightFBO;
    bool m_HPWaterFluidValid = false;
    bool m_HPWaterFluidInitialized = false;
    bool m_HPWaterFluidComputeRan = false;
    bool m_HPWaterFluidObstacleValid = false;
    bool m_HPWaterFluidHeightFieldValid = false;
    bool m_HPWaterFluidHeightCaptureRan = false;
    bool m_HPWaterFluidHeightCaptureValid = false;
    bool m_HPWaterFluidWaterHeightCaptured = false;
    bool m_HPWaterFluidSceneHeightCaptured = false;
    uint32_t m_HPWaterFluidObstacleTexture = 0;
    uint32_t m_HPWaterFluidObstacleResolution = 0;
    uint32_t m_HPWaterFluidWaterHeightTexture = 0;
    uint32_t m_HPWaterFluidSceneHeightTexture = 0;
    uint32_t m_HPWaterFluidHeightFieldResolution = 0;
    uint32_t m_HPWaterFluidHeightCaptureResolution = 0;
    uint32_t m_HPWaterFluidResolution = 0;
    glm::vec3 m_HPWaterFluidBoxCenter = glm::vec3(0.0f);
    glm::vec3 m_HPWaterFluidBoxSize = glm::vec3(1.0f);

    // Shaders
    std::shared_ptr<Shader> m_GBufferShader;
    std::shared_ptr<Shader> m_HPWaterGBufferShader;
    std::shared_ptr<Shader> m_HPWaterMaskShader;
    std::shared_ptr<Shader> m_HPWaterCompositeShader;
    std::shared_ptr<Shader> m_HPWaterVolumeShader;
    std::shared_ptr<Shader> m_HPWaterVolumeTemporalShader;
    std::shared_ptr<Shader> m_HPWaterVolumeFilterShader;
    std::shared_ptr<Shader> m_HPWaterVolumeUpsampleShader;
    std::shared_ptr<Shader> m_HPWaterCausticShader;
    std::shared_ptr<Shader> m_HPWaterCausticFilterShader;
    std::shared_ptr<Shader> m_HPWaterCausticAtlasShader;
    std::shared_ptr<ComputeShader> m_HPWaterCausticComputeShader;
    std::shared_ptr<ComputeShader> m_HPWaterFluidComputeShader;
    std::shared_ptr<Shader> m_HPWaterDepthPyramidShader;
    std::shared_ptr<Shader> m_HPWaterFluidShader;
    std::shared_ptr<Shader> m_HPWaterFluidHeightCaptureShader;
    std::shared_ptr<Shader> m_LightingShader;
    std::shared_ptr<Shader> m_DebugShader;

    // Dummy VAO for fullscreen triangle draws
    uint32_t m_QuadVAO = 0;
};

} // namespace VE
