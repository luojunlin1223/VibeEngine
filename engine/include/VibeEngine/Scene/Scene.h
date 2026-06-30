/*
 * Scene — Owns the ECS registry and manages entity lifecycle.
 *
 * The Scene is the top-level container for all game objects.
 * It provides methods to create/destroy entities and to render
 * all entities that have a MeshRendererComponent.
 */
#pragma once

#include "VibeEngine/Core/UUID.h"
#include "VibeEngine/Renderer/Texture.h"
#include "VibeEngine/Renderer/ReflectionProbe.h"
#include "VibeEngine/Renderer/ShadowMap.h"
#include "VibeEngine/Renderer/DeferredRenderer.h"
#include "VibeEngine/Physics/PhysicsWorld.h"
#include "VibeEngine/Navigation/NavGrid.h"
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <string>
#include <array>
#include <vector>
#include <utility>
#include <memory>
#include <unordered_map>

namespace VE {

class Entity;

struct RenderPipelineSettings {
    // HDR Pipeline
    bool HDREnabled = true;
    int  ToneMapMode = 1; // 0=Reinhard, 1=ACES Filmic, 2=Uncharted2
    float Exposure = 1.0f;

    // Sky
    bool SkyEnabled = true;
    std::array<float, 3> SkyTopColor    = { 0.4f, 0.7f, 1.0f };
    std::array<float, 3> SkyBottomColor = { 0.9f, 0.9f, 0.95f };
    std::shared_ptr<Texture2D> SkyTexture;
    std::string SkyTexturePath;

    // Indirect Lighting
    bool IndirectLightingEnabled = true;
    float IndirectDiffuseIntensity = 0.35f;
    float SkyReflectionIntensity = 0.20f;
    std::array<float, 3> IndirectTint = { 1.0f, 1.0f, 1.0f };

    // Bloom
    bool BloomEnabled = false;
    float BloomThreshold = 0.8f;
    float BloomIntensity = 1.0f;
    int BloomIterations = 5;

    // Vignette
    bool VignetteEnabled = false;
    float VignetteIntensity = 0.5f;
    float VignetteSmoothness = 0.5f;

    // Color Adjustments
    bool ColorAdjustEnabled = false;
    float ColorExposure = 0.0f;
    float ColorContrast = 0.0f;
    float ColorSaturation = 0.0f;
    std::array<float, 3> ColorFilter = { 1.0f, 1.0f, 1.0f };
    float ColorGamma = 1.0f;

    // Shadows / Midtones / Highlights
    bool SMHEnabled = false;
    std::array<float, 3> SMH_Shadows    = { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> SMH_Midtones   = { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> SMH_Highlights = { 1.0f, 1.0f, 1.0f };
    float SMH_ShadowStart    = 0.0f;
    float SMH_ShadowEnd      = 0.3f;
    float SMH_HighlightStart = 0.55f;
    float SMH_HighlightEnd   = 1.0f;

    // Color Curves (per-channel control points in [0,1])
    bool CurvesEnabled = false;
    std::vector<std::pair<float, float>> CurvesMaster = { {0.0f, 0.0f}, {1.0f, 1.0f} };
    std::vector<std::pair<float, float>> CurvesRed    = { {0.0f, 0.0f}, {1.0f, 1.0f} };
    std::vector<std::pair<float, float>> CurvesGreen  = { {0.0f, 0.0f}, {1.0f, 1.0f} };
    std::vector<std::pair<float, float>> CurvesBlue   = { {0.0f, 0.0f}, {1.0f, 1.0f} };

    // Tonemapping
    bool TonemapEnabled = false;
    int  TonemapMode = 2; // 0=None, 1=Reinhard, 2=ACES, 3=Uncharted2

    // Fog
    bool FogEnabled = false;
    int  FogMode    = 2;  // 0=Linear, 1=Exp, 2=Exp2
    std::array<float, 3> FogColor = { 0.7f, 0.75f, 0.8f };
    float FogDensity       = 0.02f;
    float FogStart         = 10.0f;
    float FogEnd           = 100.0f;
    float FogHeightFalloff = 0.0f;
    float FogMaxOpacity    = 1.0f;

    // Volumetric Fog
    bool  VolFogEnabled      = false;
    float VolFogDensity      = 0.015f;
    float VolFogScattering   = 0.7f;
    float VolFogLightIntensity = 1.0f;
    std::array<float, 3> VolFogColor = { 0.9f, 0.9f, 1.0f };
    int   VolFogSteps        = 32;
    float VolFogMaxDistance   = 100.0f;
    float VolFogHeightFalloff = 0.05f;
    float VolFogBaseHeight    = 0.0f;

    // Depth of Field
    bool DoFEnabled = false;
    float DoFFocusDistance = 10.0f;
    float DoFFocusRange = 5.0f;
    float DoFMaxBlur = 4.0f;
    float DoFApertureSize = 0.05f;

    // SSAO
    bool SSAOEnabled = false;
    float SSAORadius    = 0.5f;
    float SSAOBias      = 0.025f;
    float SSAOIntensity = 1.0f;
    int   SSAOKernelSize = 32;

    // SSR (Screen-Space Reflections)
    bool  SSREnabled     = false;
    int   SSRMaxSteps    = 64;
    float SSRStepSize    = 0.05f;
    float SSRThickness   = 0.1f;
    float SSRMaxDistance  = 50.0f;

    // Anti-Aliasing
    int AAMode = 0;  // 0=None, 1=MSAA 2x, 2=MSAA 4x, 3=MSAA 8x, 4=FXAA, 5=TAA
    // FXAA params
    float FXAAEdgeThreshold    = 0.0833f;
    float FXAAEdgeThresholdMin = 0.0625f;
    float FXAASubpixelQuality  = 0.75f;
    // TAA params
    float TAABlendFactor = 0.1f;  // weight of current frame (lower = more smoothing)

    // Motion Blur
    bool  MotionBlurEnabled  = false;
    float MotionBlurStrength  = 0.5f;
    int   MotionBlurSamples   = 8;

    // Occlusion Culling
    bool OcclusionCullingEnabled = false;

    // Cascaded Shadow Maps
    bool  ShadowsEnabled          = true;
    int   ShadowResolution        = 2048;   // per-cascade (512, 1024, 2048, 4096)
    float ShadowMaxDistance       = 200.0f;
    float ShadowSplitLambda       = 0.75f;  // 0=uniform, 1=logarithmic
    float ShadowDepthBias         = 0.0025f;
    float ShadowNormalBias        = 0.03f;
    int   ShadowPCFQuality        = 2;      // 0=Hard, 1=3x3, 2=5x5
    float ShadowCascadeBlendWidth = 0.1f;

    // Deferred debug
    int  GBufferDebugView = 0; // 0=None, 1..11 = GBufferDebugView enum
};

struct RenderDiagnostics {
    uint64_t FrameIndex = 0;
    uint32_t ViewportWidth = 0;
    uint32_t ViewportHeight = 0;

    uint32_t MeshRendererEntities = 0;
    uint32_t OpaqueSubmitted = 0;
    uint32_t TransparentQueued = 0;
    uint32_t TransparentDrawn = 0;
    uint32_t FrustumCulled = 0;

    uint32_t HPWaterEntities = 0;
    uint32_t HPWaterWithMesh = 0;
    uint32_t HPWaterQueued = 0;
    uint32_t HPWaterDrawn = 0;
    uint32_t HPWaterCulled = 0;
    uint32_t HPWaterGBufferDrawn = 0;
    bool HPWaterCompositeRan = false;
    bool HPWaterGBufferInitialized = false;
    uint32_t HPWaterGBufferAttachmentCount = 0;
    uint32_t HPWaterGBuffer0 = 0;
    uint32_t HPWaterGBuffer1 = 0;
    uint32_t HPWaterGBuffer2 = 0;
    uint32_t HPWaterGBufferDepth = 0;
    bool HPWaterMaskRan = false;
    uint32_t HPWaterMaskTexture = 0;
    uint32_t HPWaterMaskWidth = 0;
    uint32_t HPWaterMaskHeight = 0;
    uint32_t HPWaterCompositeTexture = 0;
    uint32_t HPWaterRefractionDataTexture = 0;
    uint32_t HPWaterRefractionMetaTexture = 0;
    bool HPWaterDepthPyramidRan = false;
    bool HPWaterDepthMergedToSceneDepth = false;
    bool HPWaterNormalMergedToSceneGBuffer = false;
    bool HPWaterStencilMarkedInSceneDepth = false;
    uint32_t HPWaterStencilRef = 0;
    uint32_t HPWaterDepthPyramidTexture = 0;
    uint32_t HPWaterDepthPyramidMipCount = 0;
    uint32_t HPWaterDepthPyramidWidth = 0;
    uint32_t HPWaterDepthPyramidHeight = 0;
    float HPWaterRefractionStrength = 0.0f;
    float HPWaterWaterDispersionStrength = 0.0f;
    float HPWaterMaxRefractionCrossDistance = 0.0f;
    float HPWaterRefractionThicknessOffset = 0.0f;
    uint32_t HPWaterRefractionSampleCount = 0;
    bool HPWaterRefractionJitterEnabled = false;
    bool HPWaterRefractionNDCMarchEnabled = false;
    float HPWaterEnvironmentReflectionIntensity = 0.0f;
    float HPWaterIndirectLightStrength = 0.0f;
    float HPWaterMacroScatterStrength = 0.0f;
    float HPWaterThinSSSStrength = 0.0f;
    float HPWaterBacklitTransmissionStrength = 0.0f;
    float HPWaterForwardScatterStrength = 0.0f;
    float HPWaterForwardScatterBlurDensity = 0.0f;
    float HPWaterMultiScatterScale = 0.0f;
    float HPWaterPhaseG = 0.0f;
    float HPWaterSpecularFGDStrength = 0.0f;
    float HPWaterGGXEnergyCompensation = 0.0f;
    bool HPWaterPreintegratedFGDLUTValid = false;
    uint32_t HPWaterPreintegratedFGDLUTTexture = 0;
    uint32_t HPWaterPreintegratedFGDLUTResolution = 0;
    bool HPWaterAreaLightLTCLUTValid = false;
    uint32_t HPWaterAreaLightLTCLUTTexture = 0;
    uint32_t HPWaterAreaLightLTCLUTResolution = 0;
    uint32_t HPWaterAreaLightLTCLUTLayers = 0;
    bool HPWaterAreaLightLTCHDRPTableEnabled = false;
    bool HPWaterAreaLightLTCSamplingEnabled = false;
    bool HPWaterAreaLightLTCHDRPUVEnabled = false;
    bool HPWaterAreaLightLTCCosThetaParamEnabled = false;
    bool HPWaterAreaLightLTCMatrixCoefficientsEnabled = false;
    bool HPWaterAreaLightLTCPolygonIntegrationEnabled = false;
    bool HPWaterAreaLightLTCHorizonClippingEnabled = false;
    bool HPWaterLightLoopInputsValid = false;
    bool HPWaterSurfaceShadowSamplingEnabled = false;
    bool HPWaterShadowCascadeDitherEnabled = false;
    bool HPWaterIndirectScatterIntegrationEnabled = false;
    bool HPWaterBSDFComponentWeightingEnabled = false;
    bool HPWaterPunctualBodyComponentWeightingEnabled = false;
    bool HPWaterSpecularSelfOcclusionEnabled = false;
    bool HPWaterExitFresnelEnabled = false;
    float HPWaterExitFresnelF0 = 0.0f;
    float HPWaterSkyReflectionIntensity = 0.0f;
    float HPWaterIndirectDiffuseIntensity = 0.0f;
    float HPWaterDirectionalLightIntensity = 0.0f;
    uint32_t HPWaterPointLightCount = 0;
    uint32_t HPWaterSpotLightCount = 0;
    uint32_t HPWaterAreaLightCount = 0;
    bool HPWaterPunctualLightLoopEnabled = false;
    bool HPWaterAreaLightApproximationEnabled = false;
    bool HPWaterAreaLightRectangleSamplingEnabled = false;
    bool HPWaterPunctualLightLayerFilteringEnabled = false;
    bool HPWaterAreaLightLayerFilteringEnabled = false;
    bool HPWaterPunctualLightInfluenceSortingEnabled = false;
    uint32_t HPWaterPunctualPointLightCandidates = 0;
    uint32_t HPWaterPunctualSpotLightCandidates = 0;
    uint32_t HPWaterAreaLightCandidates = 0;
    uint32_t HPWaterPunctualPointLightCapacity = 0;
    uint32_t HPWaterPunctualSpotLightCapacity = 0;
    uint32_t HPWaterAreaLightCapacity = 0;
    bool HPWaterLightSelectionBoundsValid = false;
    glm::vec3 HPWaterLightSelectionCenter{0.0f};
    float HPWaterLightSelectionRadius = 0.0f;
    float HPWaterPointLightTopInfluenceScore = 0.0f;
    float HPWaterSpotLightTopInfluenceScore = 0.0f;
    float HPWaterAreaLightTopInfluenceScore = 0.0f;
    float HPWaterPointLightSelectedInfluenceSum = 0.0f;
    float HPWaterSpotLightSelectedInfluenceSum = 0.0f;
    float HPWaterAreaLightSelectedInfluenceSum = 0.0f;
    uint32_t HPWaterPunctualLightsLayerSkipped = 0;
    uint32_t HPWaterPunctualLightsCapacitySkipped = 0;
    uint32_t HPWaterPunctualPointLightsCapacitySkipped = 0;
    uint32_t HPWaterPunctualSpotLightsCapacitySkipped = 0;
    uint32_t HPWaterAreaLightsLayerSkipped = 0;
    uint32_t HPWaterAreaLightsCapacitySkipped = 0;
    uint32_t HPWaterVolumePointLightCount = 0;
    uint32_t HPWaterVolumeSpotLightCount = 0;
    uint32_t HPWaterVolumeAreaLightCount = 0;
    bool HPWaterVolumePunctualLightLoopEnabled = false;
    bool HPWaterVolumeAreaLightLoopEnabled = false;
    bool HPWaterVolumeAreaLightRectangleSamplingEnabled = false;
    bool HPWaterVolumeAreaLightLTCPolygonIntegrationEnabled = false;
    bool HPWaterVolumeAreaLightLTCHorizonClippingEnabled = false;
    bool HPWaterSpectralOceanEnabled = false;
    bool HPWaterSpectralNormalParityEnabled = false;
    bool HPWaterSpectrumComputeRan = false;
    bool HPWaterSpectrumComputeValid = false;
    bool HPWaterSpectrumTextureConsumed = false;
    uint32_t HPWaterSpectrumTexture = 0;
    uint32_t HPWaterSpectrumResolution = 0;
    float HPWaterSpectrumAmplitude = 0.0f;
    float HPWaterSpectrumNormalStrength = 0.0f;
    float HPWaterSpectrumWindSpeed = 0.0f;
    float HPWaterSpectrumDirectionalSpread = 0.0f;
    float HPWaterSpectrumSwell = 0.0f;
    float HPWaterSpectrumShortWaveFade = 0.0f;
    bool HPWaterSpectrumWindModelEnabled = false;
    bool HPWaterSpectrumFrequencyDomainEnabled = false;
    bool HPWaterSpectrumPhillipsEnabled = false;
    bool HPWaterSpectrumJonswapEnabled = false;
    bool HPWaterSpectrumIFFTEnabled = false;
    int HPWaterSpectrumButterflyPasses = 0;
    bool HPWaterSkyTextureReflectionBound = false;
    uint32_t HPWaterSkyTexture = 0;
    bool HPWaterReflectionProbeBound = false;
    uint32_t HPWaterReflectionProbeTexture = 0;
    uint32_t HPWaterReflectionProbeSecondaryTexture = 0;
    float HPWaterReflectionProbeIntensity = 0.0f;
    float HPWaterReflectionProbeBlend = 0.0f;
    float HPWaterReflectionProbeInfluenceWeight = 0.0f;
    float HPWaterReflectionProbeHierarchyWeight = 0.0f;
    bool HPWaterReflectionProbeInfluenceBlendEnabled = false;
    bool HPWaterReflectionProbeBoxProjectionEnabled = false;
    bool HPWaterEnvSpecularDominantDirEnabled = false;
    bool HPWaterEnvSpecularDominantDirExactFormulaEnabled = false;
    bool HPWaterEnvSpecularMultiBounceEnabled = false;
    bool HPWaterSSRReflectionEnabled = false;
    bool HPWaterSSRHierarchyBlendEnabled = false;
    bool HPWaterSSRLightingBufferRan = false;
    bool HPWaterSSRLightingBufferValid = false;
    bool HPWaterSSRLightingRGBPreweighted = false;
    bool HPWaterSSRHitRefinementEnabled = false;
    bool HPWaterSSRRoughnessConeTracingEnabled = false;
    bool HPWaterSSRTemporalResolveEnabled = false;
    bool HPWaterSSRHistoryValid = false;
    bool HPWaterSSRExplicitMotionVectorEnabled = false;
    bool HPWaterSSRMotionVectorHistoryEnabled = false;
    bool HPWaterSSRMotionReprojectionEnabled = false;
    bool HPWaterSSRDisocclusionRejectionEnabled = false;
    bool HPWaterCompositeConsumesSSRLightingBuffer = false;
    uint32_t HPWaterSSRLightingBufferTexture = 0;
    bool HPWaterSSRDiagnosticsValid = false;
    uint32_t HPWaterSSRDiagnosticsTexture = 0;
    uint32_t HPWaterSSRMaxSteps = 0;
    float HPWaterSSRStepSize = 0.0f;
    float HPWaterSSRThickness = 0.0f;
    float HPWaterSSRMaxDistance = 0.0f;
    glm::vec3 HPWaterReflectionProbeCenter = glm::vec3(0.0f);
    glm::vec3 HPWaterReflectionProbeBoxSize = glm::vec3(0.0f);
    glm::vec3 HPWaterReflectionProbeSecondaryCenter = glm::vec3(0.0f);
    glm::vec3 HPWaterReflectionProbeSecondaryBoxSize = glm::vec3(0.0f);
    bool HPWaterForwardScatterMipEnabled = false;
    uint32_t HPWaterForwardScatterMipCount = 0;
    bool HPWaterVolumeRan = false;
    uint32_t HPWaterVolumeColorTexture = 0;
    uint32_t HPWaterVolumeTransmittanceTexture = 0;
    uint32_t HPWaterVolumeDepthTexture = 0;
    uint32_t HPWaterVolumeWidth = 0;
    uint32_t HPWaterVolumeHeight = 0;
    bool HPWaterVolumeTemporalRan = false;
    bool HPWaterVolumeTemporalNeighborhoodClampEnabled = false;
    bool HPWaterVolumeTemporalMotionReprojectionEnabled = false;
    bool HPWaterVolumeExplicitMotionVectorEnabled = false;
    bool HPWaterVolumeSceneMotionVectorEnabled = false;
    bool HPWaterVolumeObjectMotionVectorEnabled = false;
    bool HPWaterVolumeObjectMotionFieldEnabled = false;
    glm::vec3 HPWaterVolumeObjectMotionWorldOffset = glm::vec3(0.0f);
    uint32_t HPWaterVolumeObjectMotionSourceCount = 0;
    uint32_t HPWaterVolumeObjectMotionTrackedCount = 0;
    uint32_t HPWaterVolumeObjectMotionMatchedCount = 0;
    uint32_t HPWaterVolumeObjectMotionFieldCapacity = 0;
    uint32_t HPWaterVolumeObjectMotionFieldSelected = 0;
    bool HPWaterVolumeMotionVectorHistoryEnabled = false;
    bool HPWaterVolumeExponentialIntegrationEnabled = false;
    bool HPWaterVolumeSceneInScatteringEnabled = false;
    bool HPWaterVolumeAlbedoPhaseBlendEnabled = false;
    bool HPWaterVolumePhaseGEnabled = false;
    bool HPWaterVolumeShadowSamplingEnabled = false;
    bool HPWaterVolumeShadowParamsEnabled = false;
    bool HPWaterVolumeMaxCrossDistanceEnabled = false;
    bool HPWaterVolumeDynamicShadowDistanceEnabled = false;
    uint32_t HPWaterVolumeSampleCount = 0;
    float HPWaterVolumeMaxCrossDistance = 0.0f;
    float HPWaterVolumeShadowSoftness = 0.0f;
    float HPWaterVolumeShadowMinFilterSize = 0.0f;
    uint32_t HPWaterVolumeShadowBlockerSamples = 0;
    uint32_t HPWaterVolumeShadowFilterSamples = 0;
    uint32_t HPWaterVolumeMotionVectorTexture = 0;
    float HPWaterVolumeTemporalBlendFactor = 0.0f;
    bool HPWaterVolumeSpatialFilterEnabled = false;
    uint32_t HPWaterVolumeSpatialFilterIterations = 0;
    bool HPWaterVolumeMotionVectorsEnabled = false;
    float HPWaterVolumeMotionVectorVelocityScale = 0.0f;
    bool HPWaterVolumeTemporalDepthRejectionEnabled = false;
    float HPWaterVolumeTemporalDepthThreshold = 0.0f;
    bool HPWaterVolumeSpatialDepthAwareEnabled = false;
    float HPWaterVolumeSpatialDepthSensitivity = 0.0f;
    float HPWaterVolumeTemporalNeighborhoodClampStrength = 0.0f;
    bool HPWaterVolumeHistoryValid = false;
    uint32_t HPWaterVolumeHistoryColorTexture = 0;
    uint32_t HPWaterVolumeHistoryTransmittanceTexture = 0;
    uint32_t HPWaterVolumeHistoryDepthTexture = 0;
    bool HPWaterVolumeFilterRan = false;
    uint32_t HPWaterVolumeFilterIterations = 0;
    uint32_t HPWaterVolumeFilteredColorTexture = 0;
    uint32_t HPWaterVolumeFilteredTransmittanceTexture = 0;
    uint32_t HPWaterVolumeFilteredDepthTexture = 0;
    bool HPWaterVolumeUpsampleRan = false;
    uint32_t HPWaterVolumeUpsampledColorTexture = 0;
    uint32_t HPWaterVolumeUpsampledTransmittanceTexture = 0;
    uint32_t HPWaterVolumeUpsampledDepthTexture = 0;
    uint32_t HPWaterVolumeUpsampledWidth = 0;
    uint32_t HPWaterVolumeUpsampledHeight = 0;
    bool HPWaterVolumeUpsampleGatherParityEnabled = false;
    bool HPWaterVolumeUpsampleDepthAwareEnabled = false;
    bool HPWaterVolumeCompositeFullResolutionEnabled = false;
    bool HPWaterCausticRan = false;
    bool HPWaterCausticValid = false;
    uint32_t HPWaterCausticTexture = 0;
    bool HPWaterCausticComputeRan = false;
    bool HPWaterCausticComputeValid = false;
    uint32_t HPWaterCausticComputeTexture = 0;
    uint32_t HPWaterCausticComputeWidth = 0;
    uint32_t HPWaterCausticComputeHeight = 0;
    bool HPWaterCausticComputeAtomicEnabled = false;
    uint32_t HPWaterCausticComputeAtomicTexture = 0;
    bool HPWaterCausticShadowDepthConsumed = false;
    bool HPWaterCausticRGBReceiverProjectionEnabled = false;
    bool HPWaterCausticExponentialLightStepsEnabled = false;
    bool HPWaterCausticFrameDitherEnabled = false;
    bool HPWaterCausticAtlasReceiverOutputEnabled = false;
    bool HPWaterCausticCascadeBlendEnabled = false;
    bool HPWaterCausticAtlasEdgeFilterEnabled = false;
    bool HPWaterCausticGBufferAtlasConsumed = false;
    bool HPWaterCausticGBufferAtlasDecodeEnabled = false;
    bool HPWaterCausticGBufferAtlasDepthAwareEnabled = false;
    bool HPWaterCausticSpectralWeightingEnabled = false;
    bool HPWaterCausticFilterRan = false;
    bool HPWaterCausticFilteredValid = false;
    uint32_t HPWaterCausticFilteredTexture = 0;
    uint32_t HPWaterCausticFilterIterations = 0;
    bool HPWaterCausticFilterKernelParityEnabled = false;
    bool HPWaterCausticFilterComputeParityEnabled = false;
    bool HPWaterCausticFilterLDSHaloEnabled = false;
    float HPWaterCausticStrength = 0.0f;
    float HPWaterCausticScale = 0.0f;
    float HPWaterCausticDepthFade = 0.0f;
    bool HPWaterCausticTransmittanceMaskEnabled = false;
    float HPWaterCausticTransmittanceStrength = 0.0f;
    float HPWaterCausticLeakReduction = 0.0f;
    float HPWaterCausticShadowAlphaClipThreshold = 0.0f;
    float HPWaterCausticScatterBoost = 0.0f;
    bool HPWaterCausticRGBDispersion = false;
    float HPWaterCausticDispersionStrength = 0.0f;
    bool HPWaterCausticAtlasRan = false;
    bool HPWaterCausticAtlasValid = false;
    uint32_t HPWaterCausticAtlasTexture = 0;
    uint32_t HPWaterCausticGBufferAtlasTexture = 0;
    uint32_t HPWaterCausticAtlasDepthTexture = 0;
    uint32_t HPWaterCausticAtlasTileResolution = 0;
    uint32_t HPWaterCausticAtlasWidth = 0;
    uint32_t HPWaterCausticAtlasHeight = 0;
    uint32_t HPWaterCausticAtlasCascades = 0;
    uint32_t HPWaterCausticAtlasDrawn = 0;
    bool HPWaterCausticAtlasConsumed = false;
    float HPWaterCausticFilterRadius = 0.0f;
    float HPWaterCausticFilterDepthSigma = 0.0f;
    float HPWaterCausticFilterLuminanceWeight = 0.0f;
    float HPWaterCausticVolumeStrength = 0.0f;
    bool HPWaterFluidDynamicsRan = false;
    bool HPWaterFluidDynamicsValid = false;
    bool HPWaterFluidComputeRan = false;
    bool HPWaterFluidEdgeAbsorptionParityEnabled = false;
    bool HPWaterFluidSourceClampEnabled = false;
    bool HPWaterFluidWaveEquationParityEnabled = false;
    bool HPWaterFluidSampleClampParityEnabled = false;
    bool HPWaterFluidStartFrameBakeEnabled = false;
    bool HPWaterFluidHeightCaptureCacheReused = false;
    bool HPWaterFluidLayerFilteringParityEnabled = false;
    bool HPWaterFluidRenderQueueParityEnabled = false;
    bool HPWaterFluidSceneOpaqueOnlyCapture = false;
    bool HPWaterFluidWaterLayerOnlyCapture = false;
    uint32_t HPWaterFluidHeightTexture = 0;
    uint32_t HPWaterFluidResolution = 0;
    float HPWaterFluidWaveSpeed = 0.0f;
    float HPWaterFluidDamping = 0.0f;
    bool HPWaterFluidObstacleValid = false;
    uint32_t HPWaterFluidObstacleTexture = 0;
    bool HPWaterFluidHeightFieldValid = false;
    bool HPWaterFluidHeightCaptureRan = false;
    bool HPWaterFluidHeightCaptureValid = false;
    bool HPWaterFluidCaptureSpaceParityEnabled = false;
    bool HPWaterFluidDisplacedWaterHeightCapture = false;
    bool HPWaterFluidSceneGeometryHeightCapture = false;
    uint32_t HPWaterFluidWaterHeightTexture = 0;
    uint32_t HPWaterFluidSceneHeightTexture = 0;
    uint32_t HPWaterFluidCaptureMeshCandidates = 0;
    uint32_t HPWaterFluidWaterLayerCandidates = 0;
    uint32_t HPWaterFluidSceneOpaqueCandidates = 0;
    uint32_t HPWaterFluidWaterCaptureDraws = 0;
    uint32_t HPWaterFluidSceneCaptureDraws = 0;
    uint32_t HPWaterFluidWaterLayerMask = 0;
    uint32_t HPWaterFluidWaterLayerSkipped = 0;
    uint32_t HPWaterFluidSceneWaterLayerSkipped = 0;
    uint32_t HPWaterFluidTransparentSkipped = 0;
    uint32_t HPWaterFluidObstacleCount = 0;
    uint32_t HPWaterFluidObstaclePixels = 0;

    bool DeferredInitialized = false;
    bool LightingPassRan = false;
    bool ForwardPassRan = false;
    uint32_t DeferredOutputTexture = 0;
};

class Scene {
public:
    Scene() = default;
    ~Scene() = default;

    Entity CreateEntity(const std::string& name = "GameObject");
    Entity CreateEntityWithUUID(UUID uuid, const std::string& name = "GameObject");
    void DestroyEntity(Entity entity);

    // Deferred deletion — safe to call during iteration (scripts, physics callbacks, etc.)
    void QueueDestroy(Entity entity);
    void FlushDestroyQueue();

    void SetParent(entt::entity child, entt::entity parent);
    void RemoveParent(entt::entity child);

    // Compute world model matrix (walks parent chain)
    glm::mat4 GetWorldTransform(entt::entity entity) const;
    glm::vec3 GetEntityForward(entt::entity entity) const;

    // Check if entity is active in hierarchy (self + all parents must be active)
    bool IsEntityActiveInHierarchy(entt::entity entity) const;

    void OnUpdate(float deltaTime = 0.0f);
    void OnRenderSky(const glm::mat4& skyViewProjection);

    /// Deferred rendering path — the sole rendering pipeline.
    /// Fills G-buffer, runs lighting pass, then forward-renders transparent objects.
    void OnRenderDeferred(const glm::mat4& viewProjection,
                          const glm::mat4& cameraView, const glm::mat4& cameraProjection,
                          const glm::vec3& cameraPos,
                          float nearClip, float farClip,
                          uint32_t viewportWidth, uint32_t viewportHeight);

    /// Get the deferred renderer instance (creates on first access).
    DeferredRenderer& GetDeferredRenderer() { return m_DeferredRenderer; }

    const RenderDiagnostics& GetRenderDiagnostics() const { return m_RenderDiagnostics; }

    void OnRenderTerrain(const glm::mat4& viewProjection, const glm::vec3& cameraPos);
    void OnRenderSprites(const glm::mat4& viewProjection);
    void OnRenderDecals(const glm::mat4& viewProjection, const glm::mat4& viewMatrix,
                        const glm::mat4& projMatrix, uint32_t depthTexture,
                        uint32_t screenWidth, uint32_t screenHeight);
    void OnRenderUI(uint32_t screenWidth, uint32_t screenHeight,
                    float mouseX, float mouseY, bool mouseDown);

    void StartPhysics();
    void StopPhysics();
    bool IsPhysicsRunning() const { return m_PhysicsRunning; }

    void StartScripts();
    void StopScripts();

    void StartAnimations();
    void StopAnimations();

    void StartSpriteAnimations();
    void StopSpriteAnimations();

    void StartAudio();
    void StopAudio();
    void UpdateAudio(const float listenerPos[3], const float listenerForward[3], const float listenerUp[3]);

    void StartParticles();
    void StopParticles();
    void OnUpdateParticles(float dt);
    void OnRenderParticles(const glm::mat4& viewProjection, const glm::vec3& cameraPos);

    void StartVideo();
    void StopVideo();
    void UpdateVideo(float deltaTime);

    // Camera helpers — compute view/projection from CameraComponent + transform
    static glm::mat4 ComputeCameraView(const glm::mat4& worldTransform);
    static glm::mat4 ComputeCameraProjection(int projType, float fov, float size,
                                              float nearClip, float farClip, float aspectRatio);

    template<typename... Components>
    auto GetAllEntitiesWith() { return m_Registry.view<Components...>(); }

    entt::registry& GetRegistry() { return m_Registry; }

    RenderPipelineSettings& GetPipelineSettings() { return m_PipelineSettings; }
    const RenderPipelineSettings& GetPipelineSettings() const { return m_PipelineSettings; }

    // Physics queries (delegates to PhysicsWorld)
    PhysicsWorld* GetPhysicsWorld() const { return m_PhysicsWorld.get(); }

    // Reflection probes
    void BakeReflectionProbes();
    void BakeReflectionProbe(entt::entity probeEntity);

    // Navigation
    void BakeNavGrid(float cellSize = 0.5f, float worldSize = 50.0f);
    NavGrid* GetNavGrid() { return m_NavGrid.get(); }
    const NavGrid* GetNavGrid() const { return m_NavGrid.get(); }
    void UpdateNavAgents(float deltaTime);

    // Find entity by JoltBodyID (for collision callback dispatch)
    entt::entity FindEntityByBodyID(uint32_t bodyID) const;

    // Dispatch collision events to scripts
    void DispatchCollisionEvents();

    // Scene loading request (processed at end of frame)
    void RequestLoadScene(const std::string& path) { m_PendingScenePath = path; }
    const std::string& GetPendingScene() const { return m_PendingScenePath; }
    void ClearPendingScene() { m_PendingScenePath.clear(); }

private:
    std::unordered_map<uint64_t, glm::vec3> CollectHPWaterObjectCenters();

    entt::registry m_Registry;
    uint32_t m_EntityCounter = 0;
    RenderPipelineSettings m_PipelineSettings;
    RenderDiagnostics m_RenderDiagnostics;

    std::unique_ptr<PhysicsWorld> m_PhysicsWorld;
    bool  m_PhysicsRunning = false;
    float m_PhysicsAccumulator = 0.0f;

    std::string m_PendingScenePath; // scene to load at end of frame
    std::unique_ptr<NavGrid> m_NavGrid;

    // Deferred rendering
    DeferredRenderer m_DeferredRenderer;
    ShadowMap m_ShadowMap;
    glm::mat4 m_PreviousHPWaterViewProjection = glm::mat4(1.0f);
    bool m_HasPreviousHPWaterViewProjection = false;
    std::unordered_map<uint64_t, glm::vec3> m_PreviousHPWaterObjectPositions;
    bool m_HasPreviousHPWaterObjectPositions = false;

    // Deferred entity deletion queue (flushed at end of OnUpdate)
    std::vector<entt::entity> m_PendingDestroy;

    friend class Entity;
};

} // namespace VE
