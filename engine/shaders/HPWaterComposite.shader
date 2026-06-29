// VibeEngine ShaderLab - HPWater composite pass
// Consumes opaque scene color/depth and dedicated HPWater G-buffer data to
// produce the first refraction/composite slice of the HPWater pipeline.

Shader "VibeEngine/HPWaterComposite" {
    Properties {
    }

    SubShader {
        Tags { "RenderType"="Opaque" "Queue"="Overlay" "LightMode"="HPWaterComposite" }

        Pass {
            Name "HPWaterCompositePass"

            Cull Off
            ZWrite Off
            ZTest Always

            GLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag

#version 460 core

#ifdef VERTEX
layout(location = 0) out vec2 v_UV;
void main() {
    vec2 pos = vec2((gl_VertexID & 1) * 2.0, (gl_VertexID & 2) * 1.0);
    v_UV = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
#endif

#ifdef FRAGMENT
layout(location = 0) in vec2 v_UV;
layout(location = 0) out vec4 FragColor;
layout(location = 1) out vec4 RefractData;
layout(location = 2) out vec4 RefractMeta;
layout(location = 3) out vec4 SSRDiagnostics;

uniform sampler2D u_SceneColor;
uniform sampler2D u_SceneDepth;
uniform sampler2D u_HPWaterDepthPyramid;
uniform sampler2D u_HPWaterNormalRoughness;
uniform sampler2D u_HPWaterScatterThickness;
uniform sampler2D u_HPWaterAbsorptionFoam;
uniform sampler2D u_HPWaterDepth;
uniform sampler2D u_HPWaterMask;
uniform sampler2D u_HPWaterVolumeColor;
uniform sampler2D u_HPWaterVolumeTransmittance;
uniform sampler2D u_HPWaterVolumeDepth;
uniform sampler2D u_HPWaterCaustic;
uniform sampler2D u_HPWaterSSRLighting;
uniform sampler2D u_SkyTexture;
uniform sampler2D u_PreintegratedFGDLUT;
uniform sampler2DArray u_AreaLightLTCLUT;
uniform samplerCube u_ReflectionProbe;
uniform samplerCube u_ReflectionProbeSecondary;

uniform float u_NearClip;
uniform float u_FarClip;
uniform float u_RefractionStrength;
uniform float u_WaterDispersionStrength;
uniform float u_MaxRefractionCrossDistance;
uniform float u_RefractionThicknessOffset;
uniform float u_EnvironmentReflectionIntensity;
uniform float u_IndirectLightStrength;
uniform float u_ThinSSSStrength;
uniform float u_BacklitTransmissionStrength;
uniform float u_ForwardScatterStrength;
uniform float u_ForwardScatterBlurDensity;
uniform float u_MultiScatterScale;
uniform float u_PhaseG;
uniform float u_SpecularFGDStrength;
uniform float u_GGXEnergyCompensation;
uniform vec3 u_ViewPos;
uniform vec3 u_LightDir;
uniform vec3 u_LightColor;
uniform float u_LightIntensity;
uniform int u_NumPointLights;
uniform vec3 u_PointLightPositions[8];
uniform vec3 u_PointLightColors[8];
uniform float u_PointLightIntensities[8];
uniform float u_PointLightRanges[8];
uniform int u_NumSpotLights;
uniform vec3 u_SpotLightPositions[4];
uniform vec3 u_SpotLightDirections[4];
uniform vec3 u_SpotLightColors[4];
uniform float u_SpotLightIntensities[4];
uniform float u_SpotLightRanges[4];
uniform float u_SpotLightInnerCos[4];
uniform float u_SpotLightOuterCos[4];
uniform int u_NumAreaLights;
uniform vec3 u_AreaLightPositions[4];
uniform vec3 u_AreaLightRights[4];
uniform vec3 u_AreaLightUps[4];
uniform vec3 u_AreaLightForwards[4];
uniform vec3 u_AreaLightColors[4];
uniform float u_AreaLightIntensities[4];
uniform float u_AreaLightRanges[4];
uniform float u_AreaLightWidths[4];
uniform float u_AreaLightHeights[4];
uniform vec3 u_IndirectSkyColor;
uniform vec3 u_IndirectGroundColor;
uniform vec3 u_IndirectTint;
uniform int u_IndirectLightingEnabled;
uniform float u_IndirectDiffuseIntensity;
uniform float u_SkyReflectionIntensity;
uniform float u_ReflectionProbeIntensity;
uniform float u_ReflectionProbeBlend;
uniform float u_ReflectionProbeHierarchyWeight;
uniform float u_HPWaterSSRStepSize;
uniform float u_HPWaterSSRThickness;
uniform float u_HPWaterSSRMaxDistance;
uniform float u_HPWaterSSRStrength;
uniform vec3 u_ReflectionProbeCenter;
uniform vec3 u_ReflectionProbeBoxMin;
uniform vec3 u_ReflectionProbeBoxMax;
uniform vec3 u_ReflectionProbeSecondaryCenter;
uniform vec3 u_ReflectionProbeSecondaryBoxMin;
uniform vec3 u_ReflectionProbeSecondaryBoxMax;
uniform int u_HPWaterVolumeEnabled;
uniform int u_HPWaterVolumeFullResolution;
uniform int u_HPWaterCausticEnabled;
uniform int u_HPWaterDepthPyramidEnabled;
uniform int u_HasSkyTexture;
uniform int u_HasReflectionProbe;
uniform int u_ReflectionProbeBoxProjectionEnabled;
uniform int u_PreintegratedFGDLUTEnabled;
uniform int u_AreaLightLTCLUTEnabled;
uniform int u_HPWaterDepthPyramidMipCount;
uniform int u_SceneColorMipEnabled;
uniform int u_SceneColorMipCount;
uniform int u_HPWaterMaskEnabled;
uniform int u_HPWaterSSRLightingEnabled;
uniform int u_RefractionSampleCount;
uniform int u_RefractionJitterEnabled;
uniform int u_HPWaterSSREnabled;
uniform int u_HPWaterSSRMaxSteps;
uniform int u_FrameIndex;
uniform mat4 u_ViewProjection;
uniform mat4 u_InverseViewProjection;

#include "shadows.glslinc"

const float PI = 3.14159265358979323846;
const float HPWATER_FORWARD_SCATTER_BLUR_DENSITY_SCALE = 10.0;
const float HPWATER_FORWARD_SCALING_FACTOR = 1.0;
const float HPWATER_WATER_DISPERSION_UV_CLAMP = 0.01;
const int HPWATER_INDIRECT_SAMPLE_COUNT = 16;
const float HPWATER_INDIRECT_EXP_FACTOR = 32.0;
const float HPWATER_SSS_PATH_SCALE = 20.0;
const float HPWATER_SSS_NONLINEAR_STRENGTH = 0.5;
const float HPWATER_SSS_SCATTER_BOOST = 2.0;
const float HPWATER_BACKLIT_PATH_SCALE = 20.0;
const float HPWATER_WATER_F0 = 0.02037;

float LinearizeDepth(float depth) {
    float z = depth * 2.0 - 1.0;
    return (2.0 * u_NearClip * u_FarClip) /
        max(u_FarClip + u_NearClip - z * (u_FarClip - u_NearClip), 0.0001);
}

vec3 ReconstructWorldPosition(vec2 uv, float depth) {
    vec2 ndcXY = uv * 2.0 - 1.0;
    float ndcZ = depth * 2.0 - 1.0;
    vec4 world = u_InverseViewProjection * vec4(ndcXY, ndcZ, 1.0);
    float invW = abs(world.w) > 0.00001 ? 1.0 / world.w : 0.0;
    return world.xyz * invW;
}

vec3 NormalizeOr(vec3 v, vec3 fallback) {
    float len2 = dot(v, v);
    return len2 > 0.000001 ? v * inversesqrt(len2) : fallback;
}

vec3 ProjectWorldToNDCLinear(vec3 worldPos) {
    vec4 clip = u_ViewProjection * vec4(worldPos, 1.0);
    if (clip.w <= 0.00001) {
        return vec3(-1.0);
    }

    vec3 ndc = clip.xyz / clip.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    float rawDepth = ndc.z * 0.5 + 0.5;
    return vec3(uv, LinearizeDepth(clamp(rawDepth, 0.0, 1.0)));
}

vec3 ComputeHPWaterRefractionDirection(vec3 waterWorldPos, vec3 sceneWorldPos, vec3 waterNormal) {
    const float eta = 1.0 / 1.33;
    vec3 cameraToWater = NormalizeOr(waterWorldPos - u_ViewPos, vec3(0.0, 0.0, 1.0));
    vec3 waterCrossDir = NormalizeOr(sceneWorldPos - waterWorldPos, cameraToWater);
    vec3 normalGain = vec3(clamp(u_RefractionStrength, 0.0, 2.0), 1.0, clamp(u_RefractionStrength, 0.0, 2.0));
    vec3 refracted = refract(cameraToWater, normalize(waterNormal * normalGain), eta);
    vec3 flatRefracted = refract(cameraToWater, vec3(0.0, 1.0, 0.0), eta);

    if (dot(refracted, refracted) <= 0.000001) {
        return vec3(0.0);
    }

    vec3 bent = refracted;
    if (dot(flatRefracted, flatRefracted) > 0.000001) {
        bent = refracted - flatRefracted + waterCrossDir;
    }

    return NormalizeOr(bent, waterCrossDir);
}

float ScreenEdgeFade(vec2 uv) {
    vec2 edge = min(uv, vec2(1.0) - uv);
    return clamp(min(edge.x, edge.y) * 8.0, 0.0, 1.0);
}

float InterleavedGradientNoise(vec2 pixelPos, int frameIndex) {
    const vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
    vec2 scrolled = pixelPos + vec2(float(frameIndex & 63)) * vec2(5.588238, 5.588238);
    return fract(magic.z * fract(dot(scrolled, magic.xy)));
}

float RefractionStepJitter(vec2 uv) {
    if (u_RefractionJitterEnabled != 1) {
        return 0.0;
    }
    ivec2 sceneSize = textureSize(u_SceneDepth, 0);
    vec2 pixel = uv * vec2(max(sceneSize, ivec2(1)));
    return InterleavedGradientNoise(pixel, u_FrameIndex);
}

float SchlickFresnel(float cosTheta, float f0) {
    float f = clamp(1.0 - cosTheta, 0.0, 1.0);
    float f2 = f * f;
    return f0 + (1.0 - f0) * f2 * f2 * f;
}

float HenyeyGreenstein(float cosTheta, float g) {
    float g2 = g * g;
    float denom = max(1.0 + g2 - 2.0 * g * cosTheta, 0.001);
    return (1.0 - g2) / (4.0 * PI * pow(denom, 1.5));
}

float HPWaterHenyeyPhase(float cosTheta, float g) {
    float g2 = g * g;
    float denom = max(1.0 + g2 - 2.0 * g * cosTheta, 0.0001);
    return min((1.0 - g2) / pow(denom, 1.5), 64.0);
}

vec3 HPWaterScatterPhase(float cosTheta, float phaseG) {
    const vec3 betaRayleigh = vec3(5.8e-6, 13.5e-6, 33.1e-6);
    float rayleighPhase = (1.0 + cosTheta * cosTheta) * (3.0 / (16.0 * PI));
    vec3 rayleighScatter = betaRayleigh * rayleighPhase * 1.0e6;
    float mieScatter = HPWaterHenyeyPhase(cosTheta, phaseG);
    return rayleighScatter * 0.05 + vec3(mieScatter) * 0.95;
}

float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    return a2 / max(PI * denom * denom, 0.0001);
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / max(NdotV * (1.0 - k) + k, 0.0001);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

vec3 FresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) *
        pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec2 EnvBRDFApprox(float roughness, float NdotV) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}

vec3 ApplyFGD(vec3 fresnel, vec3 F0, float roughness, float NdotV) {
    vec2 brdf = u_PreintegratedFGDLUTEnabled == 1
        ? texture(u_PreintegratedFGDLUT, vec2(clamp(NdotV, 0.0, 1.0), clamp(roughness, 0.0, 1.0))).rg
        : EnvBRDFApprox(roughness, NdotV);
    vec3 splitSum = clamp(F0 * brdf.x + vec3(brdf.y), vec3(0.0), vec3(1.0));
    return mix(fresnel, splitSum, clamp(u_SpecularFGDStrength, 0.0, 1.0));
}

float GGXEnergyCompensation(vec3 F0, float roughness) {
    float avgF0 = clamp((F0.r + F0.g + F0.b) * 0.3333333, 0.0, 1.0);
    float roughnessLoss = 1.0 - clamp(roughness, 0.0, 1.0);
    float compensation = 1.0 + avgF0 * roughnessLoss * 2.0;
    return mix(1.0, compensation, clamp(u_GGXEnergyCompensation, 0.0, 2.0) * 0.5);
}

vec3 SampleIndirectSky(vec3 dir) {
    float t = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
    return mix(u_IndirectGroundColor, u_IndirectSkyColor, t) * u_IndirectTint;
}

vec3 HPWaterScatteredLight(vec3 originLight,
                           vec3 absorptionCoeff,
                           vec3 scatteringCoeff,
                           float crossDistance,
                           vec3 phase) {
    vec3 extinctionCoeff = max(absorptionCoeff + scatteringCoeff, vec3(0.00001));
    vec3 transmittance = exp(-extinctionCoeff * max(crossDistance, 0.00001));
    vec3 extinguishedLight = originLight * (vec3(1.0) - transmittance);
    vec3 scatteringAlbedo = scatteringCoeff / extinctionCoeff;
    return extinguishedLight * scatteringAlbedo * phase;
}

vec3 HPWaterScatteredLightWithTransmittance(vec3 originLight,
                                            vec3 absorptionCoeff,
                                            vec3 scatteringCoeff,
                                            float crossDistance,
                                            vec3 phase,
                                            out vec3 transmittance) {
    vec3 extinctionCoeff = max(absorptionCoeff + scatteringCoeff, vec3(0.00001));
    transmittance = exp(-extinctionCoeff * max(crossDistance, 0.00001));
    vec3 extinguishedLight = originLight * (vec3(1.0) - transmittance);
    vec3 scatteringAlbedo = scatteringCoeff / extinctionCoeff;
    return extinguishedLight * scatteringAlbedo * phase;
}

vec3 AccumulateHPWaterIndirectScattering(vec3 indirectLighting,
                                         vec3 absorptionCoeff,
                                         vec3 scatteringCoeff,
                                         float rayLength,
                                         float ambientDepth,
                                         float dither) {
    float rcpCount = 1.0 / float(HPWATER_INDIRECT_SAMPLE_COUNT);
    float kDenom = 1.0 / max(HPWATER_INDIRECT_EXP_FACTOR - 1.0, 0.0001);
    float kDD = log(HPWATER_INDIRECT_EXP_FACTOR) * rcpCount * kDenom;
    float expStep = pow(HPWATER_INDIRECT_EXP_FACTOR, rcpCount);
    float currentExp = pow(HPWATER_INDIRECT_EXP_FACTOR, clamp(dither, 0.0, 1.0) * rcpCount);
    float travelDistance = max(rayLength + ambientDepth, 0.00001);
    vec3 total = vec3(0.0);
    for (int i = 0; i < HPWATER_INDIRECT_SAMPLE_COUNT; ++i) {
        float dd = currentExp * kDD;
        float crossDistance = max(dd * travelDistance, 0.00001);
        total += HPWaterScatteredLight(indirectLighting,
                                       absorptionCoeff,
                                       scatteringCoeff,
                                       crossDistance,
                                       vec3(1.0));
        currentExp *= expStep;
    }
    return total;
}

vec2 DirectionToEquirectUV(vec3 dir) {
    vec3 d = normalize(dir);
    float u = atan(d.z, d.x) / (2.0 * PI) + 0.5;
    float v = asin(clamp(d.y, -1.0, 1.0)) / PI + 0.5;
    return vec2(u, v);
}

vec3 SampleSkyTexture(vec3 dir, float roughness) {
    vec2 uv = DirectionToEquirectUV(dir);
    int levels = textureQueryLevels(u_SkyTexture);
    float maxMip = float(max(levels - 1, 0));
    float lod = clamp(roughness * maxMip, 0.0, maxMip);
    return textureLod(u_SkyTexture, uv, lod).rgb * u_IndirectTint;
}

vec3 BoxProjectedReflectionDirection(vec3 worldPos, vec3 dir, vec3 probeCenter, vec3 boxMin, vec3 boxMax) {
    vec3 r = NormalizeOr(dir, vec3(0.0, 1.0, 0.0));
    vec3 safeR = vec3(
        abs(r.x) < 0.0001 ? (r.x < 0.0 ? -0.0001 : 0.0001) : r.x,
        abs(r.y) < 0.0001 ? (r.y < 0.0 ? -0.0001 : 0.0001) : r.y,
        abs(r.z) < 0.0001 ? (r.z < 0.0 ? -0.0001 : 0.0001) : r.z);

    vec3 t0 = (boxMin - worldPos) / safeR;
    vec3 t1 = (boxMax - worldPos) / safeR;
    vec3 tFar = max(t0, t1);
    float t = min(min(tFar.x, tFar.y), tFar.z);
    if (t <= 0.0001 || t > 100000.0) {
        return r;
    }

    return NormalizeOr(worldPos + r * t - probeCenter, r);
}

vec3 HPWaterSpecularEnvironmentDirection(vec3 normal,
                                         vec3 reflectionDir,
                                         float roughness,
                                         float nDotV) {
    vec3 N = NormalizeOr(normal, vec3(0.0, 1.0, 0.0));
    vec3 R = NormalizeOr(reflectionDir, N);

    // Unity HDRP GetSpecularDominantDir() expects perceptual roughness.
    float p = clamp(sqrt(clamp(roughness, 0.0, 1.0)), 0.0, 1.0);
    float a = max(1.0 - p * p, 0.0);
    float s = sqrt(a);
    float dominantWeight = (s + p * p) *
        clamp(a * a + mix(0.0, a, clamp(nDotV, 0.0, 1.0) * clamp(nDotV, 0.0, 1.0)), 0.0, 1.0);
    vec3 dominantR = NormalizeOr(mix(N, R, clamp(dominantWeight, 0.0, 1.0)), R);

    float roughnessBlend = smoothstep(0.0, 1.0, roughness * roughness);
    vec3 hpWaterR = NormalizeOr(mix(dominantR, R, roughnessBlend), R);
    hpWaterR.y = abs(hpWaterR.y) + 0.1;
    return NormalizeOr(hpWaterR, R);
}

vec3 SampleEnvironment(vec3 dir,
                       vec3 fallbackDir,
                       vec3 worldPos,
                       vec3 normal,
                       vec3 viewDir,
                       float roughness,
                       bool diffuseSample) {
    vec3 sampleDir = diffuseSample
        ? NormalizeOr(dir, fallbackDir)
        : HPWaterSpecularEnvironmentDirection(normal, dir, roughness, dot(normal, viewDir));

    vec3 fallback;
    if (u_HasSkyTexture == 1) {
        float skyRoughness = diffuseSample ? 1.0 : roughness;
        fallback = SampleSkyTexture(sampleDir, skyRoughness);
    } else {
        fallback = SampleIndirectSky(diffuseSample ? fallbackDir : sampleDir);
    }

    if (u_HasReflectionProbe == 1) {
        int levels = textureQueryLevels(u_ReflectionProbe);
        float maxMip = float(max(levels - 1, 0));
        float lod = diffuseSample ? maxMip : clamp(roughness * maxMip, 0.0, maxMip);
        vec3 primaryDir = normalize(sampleDir);
        vec3 secondaryDir = primaryDir;
        if (u_ReflectionProbeBoxProjectionEnabled == 1 && !diffuseSample) {
            primaryDir = BoxProjectedReflectionDirection(
                worldPos, primaryDir, u_ReflectionProbeCenter, u_ReflectionProbeBoxMin, u_ReflectionProbeBoxMax);
            secondaryDir = BoxProjectedReflectionDirection(
                worldPos, secondaryDir, u_ReflectionProbeSecondaryCenter,
                u_ReflectionProbeSecondaryBoxMin, u_ReflectionProbeSecondaryBoxMax);
        }
        vec3 primary = textureLod(u_ReflectionProbe, primaryDir, lod).rgb;
        vec3 secondary = textureLod(u_ReflectionProbeSecondary, secondaryDir, lod).rgb;
        vec3 probe = mix(primary, secondary, clamp(u_ReflectionProbeBlend, 0.0, 1.0));
        return mix(fallback, probe, clamp(u_ReflectionProbeHierarchyWeight, 0.0, 1.0));
    }

    return fallback;
}

vec3 SampleHPWaterSpecularEnvironmentHierarchy(vec3 dir,
                                               vec3 worldPos,
                                               vec3 normal,
                                               vec3 viewDir,
                                               float roughness,
                                               float consumedSSRWeight,
                                               out float consumedProbeWeight,
                                               out float consumedSkyWeight) {
    vec3 sampleDir = HPWaterSpecularEnvironmentDirection(
        normal, dir, roughness, dot(normal, viewDir));

    vec3 sky;
    if (u_HasSkyTexture == 1) {
        sky = SampleSkyTexture(sampleDir, roughness);
    } else {
        sky = SampleIndirectSky(sampleDir);
    }

    float remainingWeight = clamp(1.0 - clamp(consumedSSRWeight, 0.0, 1.0), 0.0, 1.0);
    consumedProbeWeight = 0.0;
    consumedSkyWeight = remainingWeight;

    if (u_HasReflectionProbe != 1 || remainingWeight <= 0.0001) {
        return sky * consumedSkyWeight;
    }

    int levels = textureQueryLevels(u_ReflectionProbe);
    float maxMip = float(max(levels - 1, 0));
    float lod = clamp(roughness * maxMip, 0.0, maxMip);
    vec3 primaryDir = normalize(sampleDir);
    vec3 secondaryDir = primaryDir;
    if (u_ReflectionProbeBoxProjectionEnabled == 1) {
        primaryDir = BoxProjectedReflectionDirection(
            worldPos, primaryDir, u_ReflectionProbeCenter, u_ReflectionProbeBoxMin, u_ReflectionProbeBoxMax);
        secondaryDir = BoxProjectedReflectionDirection(
            worldPos, secondaryDir, u_ReflectionProbeSecondaryCenter,
            u_ReflectionProbeSecondaryBoxMin, u_ReflectionProbeSecondaryBoxMax);
    }

    vec3 primary = textureLod(u_ReflectionProbe, primaryDir, lod).rgb;
    vec3 secondary = textureLod(u_ReflectionProbeSecondary, secondaryDir, lod).rgb;
    vec3 probe = mix(primary, secondary, clamp(u_ReflectionProbeBlend, 0.0, 1.0));
    float requestedProbeWeight = clamp(u_ReflectionProbeHierarchyWeight, 0.0, 1.0);

    consumedProbeWeight = min(requestedProbeWeight, remainingWeight);
    consumedSkyWeight = max(remainingWeight - consumedProbeWeight, 0.0);
    return probe * consumedProbeWeight + sky * consumedSkyWeight;
}

float SampleSceneDepth(vec2 uv, float lod) {
    if (u_HPWaterDepthPyramidEnabled == 1) {
        return textureLod(u_HPWaterDepthPyramid, uv, lod).r;
    }
    return texture(u_SceneDepth, uv).r;
}

float DepthPyramidLOD(float normalizedDistance, float maxTravel) {
    if (u_HPWaterDepthPyramidEnabled != 1 || u_HPWaterDepthPyramidMipCount <= 1) {
        return 0.0;
    }

    ivec2 pyramidSize = textureSize(u_HPWaterDepthPyramid, 0);
    float pixelTravel = maxTravel * float(max(pyramidSize.x, pyramidSize.y));
    float projectedFootprint = max(pixelTravel * normalizedDistance * 0.45, 1.0);
    return clamp(log2(projectedFootprint), 0.0, float(u_HPWaterDepthPyramidMipCount - 1));
}

vec3 SampleSceneColorBlurred(vec2 uv, float lod) {
    if (u_SceneColorMipEnabled != 1 || u_SceneColorMipCount <= 1) {
        return texture(u_SceneColor, uv).rgb;
    }

    float maxLod = float(max(u_SceneColorMipCount - 1, 0));
    return textureLod(u_SceneColor, uv, clamp(lod, 0.0, maxLod)).rgb;
}

float HPWaterSpecularSelfOcclusion(float nDotL) {
    // HPWaterBSDFLibary gates direct specular with saturate(clampedNdotL * 5).
    return clamp(nDotL * 5.0, 0.0, 1.0);
}

vec3 EvaluateHPWaterSpecularLight(vec3 N,
                                  vec3 V,
                                  vec3 L,
                                  vec3 radiance,
                                  float roughness,
                                  vec3 fresnel,
                                  float energyCompensation) {
    float NdotV = clamp(dot(N, V), 0.0, 1.0);
    float NdotL = clamp(dot(N, L), 0.0, 1.0);
    if (NdotL <= 0.0001 || NdotV <= 0.0001) {
        return vec3(0.0);
    }

    vec3 H = NormalizeOr(V + L, N);
    float D = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    float specularSelfOcclusion = HPWaterSpecularSelfOcclusion(NdotL);
    return (D * G * fresnel) / max(4.0 * NdotV * NdotL, 0.001) *
        radiance * NdotL * specularSelfOcclusion * energyCompensation;
}

vec4 SampleHPWaterAreaLightLTC(float roughness, float nDotV, int layer) {
    if (u_AreaLightLTCLUTEnabled != 1) {
        return vec4(1.0, 0.0, 1.0, 0.0);
    }

    ivec3 lutDims = max(textureSize(u_AreaLightLTCLUT, 0), ivec3(1));
    vec2 lutSize = vec2(lutDims.xy);
    float clampedNdotV = clamp(nDotV, 0.0, 1.0);
    float cosThetaParam = sqrt(max(1.0 - clampedNdotV, 0.0));
    vec2 uv = vec2(
        clamp(sqrt(max(roughness, 0.0)), 0.0, 1.0),
        cosThetaParam);
    uv = (uv * (lutSize - vec2(1.0)) + vec2(0.5)) / lutSize;
    float clampedLayer = float(clamp(layer, 0, max(lutDims.z - 1, 0)));
    return texture(u_AreaLightLTCLUT, vec3(uv, clampedLayer));
}

mat3 HPWaterLTCInverseMatrix(vec4 ltc) {
    return mat3(
        vec3(ltc.r, 0.0, ltc.a),
        vec3(0.0, ltc.b, 0.0),
        vec3(ltc.g, 0.0, 1.0));
}

mat3 HPWaterLTCViewNormalBasis(vec3 N, vec3 V) {
    vec3 tangent = V - N * dot(V, N);
    if (dot(tangent, tangent) < 0.0001) {
        vec3 helper = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        tangent = cross(helper, N);
    }

    tangent = NormalizeOr(tangent, vec3(1.0, 0.0, 0.0));
    vec3 bitangent = NormalizeOr(cross(N, tangent), vec3(0.0, 1.0, 0.0));
    return mat3(tangent, bitangent, N);
}

vec3 HPWaterLTCComputeEdgeFactor(vec3 v1, vec3 v2) {
    float cosTheta = clamp(dot(v1, v2), -0.9999, 0.9999);
    float theta = acos(cosTheta);
    float invSinTheta = inversesqrt(max(1.0 - cosTheta * cosTheta, 0.000001));
    return cross(v1, v2) * theta * invSinTheta;
}

vec3 HPWaterLTCPolygonFormFactor(vec3 l0, vec3 l1, vec3 l2, vec3 l3, vec3 l4, int vertexCount) {
    l0 = NormalizeOr(l0, vec3(0.0, 0.0, 1.0));
    l1 = NormalizeOr(l1, vec3(0.0, 0.0, 1.0));
    l2 = NormalizeOr(l2, vec3(0.0, 0.0, 1.0));

    if (vertexCount == 3) {
        l3 = l0;
    } else if (vertexCount == 4) {
        l3 = NormalizeOr(l3, vec3(0.0, 0.0, 1.0));
        l4 = l0;
    } else {
        l3 = NormalizeOr(l3, vec3(0.0, 0.0, 1.0));
        l4 = NormalizeOr(l4, vec3(0.0, 0.0, 1.0));
    }

    vec3 formFactor =
        HPWaterLTCComputeEdgeFactor(l0, l1) +
        HPWaterLTCComputeEdgeFactor(l1, l2) +
        HPWaterLTCComputeEdgeFactor(l2, l3);
    if (vertexCount >= 4) {
        formFactor += HPWaterLTCComputeEdgeFactor(l3, l4);
    }
    if (vertexCount == 5) {
        formFactor += HPWaterLTCComputeEdgeFactor(l4, l0);
    }
    return formFactor * (0.5 / PI);
}

float HPWaterLTCPolygonIrradiance(vec3 p0, vec3 p1, vec3 p2, vec3 p3) {
    int config = 0;
    if (p0.z > 0.0) config += 1;
    if (p1.z > 0.0) config += 2;
    if (p2.z > 0.0) config += 4;
    if (p3.z > 0.0) config += 8;

    vec3 l4 = p3;
    int vertexCount = 0;
    switch (config) {
    case 0:
        break;
    case 1:
        vertexCount = 3;
        p1 = -p1.z * p0 + p0.z * p1;
        p2 = -p3.z * p0 + p0.z * p3;
        break;
    case 2:
        vertexCount = 3;
        p0 = -p0.z * p1 + p1.z * p0;
        p2 = -p2.z * p1 + p1.z * p2;
        break;
    case 3:
        vertexCount = 4;
        p2 = -p2.z * p1 + p1.z * p2;
        p3 = -p3.z * p0 + p0.z * p3;
        break;
    case 4:
        vertexCount = 3;
        p0 = -p3.z * p2 + p2.z * p3;
        p1 = -p1.z * p2 + p2.z * p1;
        break;
    case 5:
        break;
    case 6:
        vertexCount = 4;
        p0 = -p0.z * p1 + p1.z * p0;
        p3 = -p3.z * p2 + p2.z * p3;
        break;
    case 7:
        vertexCount = 5;
        l4 = -p3.z * p0 + p0.z * p3;
        p3 = -p3.z * p2 + p2.z * p3;
        break;
    case 8:
        vertexCount = 3;
        p0 = -p0.z * p3 + p3.z * p0;
        p1 = -p2.z * p3 + p3.z * p2;
        p2 = p3;
        break;
    case 9:
        vertexCount = 4;
        p1 = -p1.z * p0 + p0.z * p1;
        p2 = -p2.z * p3 + p3.z * p2;
        break;
    case 10:
        break;
    case 11:
        vertexCount = 5;
        p3 = -p2.z * p3 + p3.z * p2;
        p2 = -p2.z * p1 + p1.z * p2;
        break;
    case 12:
        vertexCount = 4;
        p1 = -p1.z * p2 + p2.z * p1;
        p0 = -p0.z * p3 + p3.z * p0;
        break;
    case 13:
        vertexCount = 5;
        p3 = p2;
        p2 = -p1.z * p2 + p2.z * p1;
        p1 = -p1.z * p0 + p0.z * p1;
        break;
    case 14:
        vertexCount = 5;
        l4 = -p0.z * p3 + p3.z * p0;
        p0 = -p0.z * p1 + p1.z * p0;
        break;
    case 15:
        vertexCount = 4;
        break;
    }

    if (vertexCount == 0) {
        return 0.0;
    }

    vec3 formFactor = HPWaterLTCPolygonFormFactor(p0, p1, p2, p3, l4, vertexCount);
    return clamp(max(0.0, formFactor.z), 0.0, 1.0);
}

float HPWaterEvaluateAreaLightLTCPolygon(vec3 positionWS,
                                         mat3 basis,
                                         mat3 inverseLTC,
                                         vec3 p0,
                                         vec3 p1,
                                         vec3 p2,
                                         vec3 p3) {
    vec3 v0 = p0 - positionWS;
    vec3 v1 = p1 - positionWS;
    vec3 v2 = p2 - positionWS;
    vec3 v3 = p3 - positionWS;
    vec3 l0 = vec3(dot(v0, basis[0]), dot(v0, basis[1]), dot(v0, basis[2]));
    vec3 l1 = vec3(dot(v1, basis[0]), dot(v1, basis[1]), dot(v1, basis[2]));
    vec3 l2 = vec3(dot(v2, basis[0]), dot(v2, basis[1]), dot(v2, basis[2]));
    vec3 l3 = vec3(dot(v3, basis[0]), dot(v3, basis[1]), dot(v3, basis[2]));
    return HPWaterLTCPolygonIrradiance(
        inverseLTC * l0,
        inverseLTC * l1,
        inverseLTC * l2,
        inverseLTC * l3);
}

vec3 ComputeHPWaterAreaLightRadiance(vec3 positionWS,
                                     vec3 N,
                                     vec3 V,
                                     float roughness,
                                     int lightIndex,
                                     out vec3 L,
                                     out float diffuseScale) {
    vec3 center = u_AreaLightPositions[lightIndex];
    vec3 right = NormalizeOr(u_AreaLightRights[lightIndex], vec3(1.0, 0.0, 0.0));
    vec3 up = NormalizeOr(u_AreaLightUps[lightIndex], vec3(0.0, 1.0, 0.0));
    vec3 forward = NormalizeOr(u_AreaLightForwards[lightIndex], vec3(0.0, 0.0, 1.0));
    float width = max(u_AreaLightWidths[lightIndex], 0.001);
    float height = max(u_AreaLightHeights[lightIndex], 0.001);
    float range = max(u_AreaLightRanges[lightIndex], 0.001);
    vec2 halfSize = vec2(width, height) * 0.5;
    vec3 centerVector = center - positionWS;
    float distanceToLight = length(centerVector);
    if (distanceToLight <= 0.0001 || distanceToLight >= range) {
        L = forward;
        diffuseScale = 1.0;
        return vec3(0.0);
    }

    L = centerVector / distanceToLight;
    float range01 = clamp(distanceToLight / range, 0.0, 1.0);
    float rangeWindow = 1.0 - pow(range01, 4.0);
    rangeWindow *= rangeWindow;
    float emissionFacing = clamp(dot(forward, -L), 0.0, 1.0);
    if (emissionFacing <= 0.0001) {
        diffuseScale = 1.0;
        return vec3(0.0);
    }

    float nDotV = clamp(dot(N, V), 0.0, 1.0);
    vec4 ltc = SampleHPWaterAreaLightLTC(roughness, nDotV, 0);
    vec4 disneyLtc = SampleHPWaterAreaLightLTC(roughness, nDotV, 1);

    vec3 p0 = center - right * halfSize.x - up * halfSize.y;
    vec3 p1 = center + right * halfSize.x - up * halfSize.y;
    vec3 p2 = center + right * halfSize.x + up * halfSize.y;
    vec3 p3 = center - right * halfSize.x + up * halfSize.y;
    mat3 basis = HPWaterLTCViewNormalBasis(N, V);
    float specIrradiance = HPWaterEvaluateAreaLightLTCPolygon(
        positionWS, basis, HPWaterLTCInverseMatrix(ltc), p0, p1, p2, p3);
    float diffuseIrradiance = HPWaterEvaluateAreaLightLTCPolygon(
        positionWS, basis, HPWaterLTCInverseMatrix(disneyLtc), p0, p1, p2, p3);
    if (specIrradiance <= 0.0001) {
        diffuseScale = 1.0;
        return vec3(0.0);
    }

    diffuseScale = clamp(diffuseIrradiance / max(specIrradiance, 0.001), 0.0, 1.5);
    float attenuation = rangeWindow * emissionFacing * specIrradiance;
    return u_AreaLightColors[lightIndex] *
        max(u_AreaLightIntensities[lightIndex], 0.0) * attenuation;
}

vec4 TraceHPWaterSSR(vec3 waterWorldPos, vec3 normal, vec3 viewDir, float roughness, float waterLinearDepth) {
    if (u_HPWaterSSREnabled != 1 || u_HPWaterSSRMaxSteps <= 0) {
        return vec4(0.0);
    }

    vec3 R = NormalizeOr(reflect(-viewDir, normal), vec3(0.0, 1.0, 0.0));
    if (dot(R, normal) <= -0.15) {
        return vec4(0.0);
    }

    int maxSteps = clamp(u_HPWaterSSRMaxSteps, 1, 128);
    float stepSize = clamp(u_HPWaterSSRStepSize, 0.005, 5.0);
    float thickness = clamp(u_HPWaterSSRThickness, 0.005, 5.0);
    float maxDistance = clamp(u_HPWaterSSRMaxDistance, 0.1, 500.0);
    vec3 start = waterWorldPos + normal * max(thickness * 0.25, 0.01);
    float travelled = stepSize;

    for (int i = 0; i < 128; ++i) {
        if (i >= maxSteps || travelled > maxDistance) {
            break;
        }

        float progress = float(i) / float(max(maxSteps - 1, 1));
        vec3 rayPos = start + R * travelled;
        vec4 clip = u_ViewProjection * vec4(rayPos, 1.0);
        if (clip.w <= 0.00001) {
            travelled += stepSize * mix(1.0, 2.25, progress);
            continue;
        }

        vec3 ndc = clip.xyz / clip.w;
        vec2 uv = ndc.xy * 0.5 + 0.5;
        if (uv.x <= 0.001 || uv.y <= 0.001 || uv.x >= 0.999 || uv.y >= 0.999 ||
            ndc.z <= -1.0 || ndc.z >= 1.0) {
            break;
        }

        float rayDepth = ndc.z * 0.5 + 0.5;
        float rayLinear = LinearizeDepth(clamp(rayDepth, 0.0, 1.0));
        if (rayLinear <= waterLinearDepth + 0.01) {
            travelled += stepSize * mix(1.0, 2.25, progress);
            continue;
        }

        float sceneDepth = texture(u_SceneDepth, uv).r;
        if (sceneDepth < 0.9999) {
            float sceneLinear = LinearizeDepth(sceneDepth);
            float delta = rayLinear - sceneLinear;
            float adaptiveThickness = thickness + travelled * 0.015;
            if (delta >= -adaptiveThickness * 0.25 && delta <= adaptiveThickness) {
                float edgeFade = ScreenEdgeFade(uv);
                float distanceFade = 1.0 - smoothstep(maxDistance * 0.35, maxDistance, travelled);
                float roughnessFade = mix(1.0, 0.35, clamp(roughness, 0.0, 1.0));
                float confidence = edgeFade * distanceFade * roughnessFade *
                    clamp(u_HPWaterSSRStrength, 0.0, 1.0);
                float lod = roughness * float(max(u_SceneColorMipCount - 1, 0));
                vec3 color = SampleSceneColorBlurred(uv, lod);
                return vec4(color, confidence);
            }
        }

        travelled += stepSize * mix(1.0, 2.25, progress);
    }

    return vec4(0.0);
}

vec2 HPWaterDispersionJitter(vec2 uv) {
    ivec2 sceneSize = max(textureSize(u_SceneColor, 0), ivec2(1));
    vec2 pixel = uv * vec2(sceneSize);
    float noiseA = InterleavedGradientNoise(pixel, u_FrameIndex);
    float noiseB = InterleavedGradientNoise(pixel + vec2(17.0, 29.0), u_FrameIndex + 11);
    float angle = noiseA * (PI * 2.0) + float(u_FrameIndex & 63) * 2.39996323;
    float radius = sqrt(clamp(noiseB, 0.0, 1.0));
    return vec2(cos(angle), sin(angle)) * radius;
}

vec3 SampleHPWaterDispersedSceneColor(vec2 baseUV, vec2 refractedUV, float lod) {
    float dispersionStrength = clamp(u_WaterDispersionStrength, 0.0, 2.0);
    if (dispersionStrength <= 0.0001) {
        return SampleSceneColorBlurred(refractedUV, lod);
    }

    vec2 refractionDeltaUV = refractedUV - baseUV;
    vec2 dispersionUV = clamp(
        refractionDeltaUV * dispersionStrength,
        vec2(-HPWATER_WATER_DISPERSION_UV_CLAMP),
        vec2( HPWATER_WATER_DISPERSION_UV_CLAMP));
    dispersionUV *= HPWaterDispersionJitter(baseUV);

    vec3 sampleLong = SampleSceneColorBlurred(clamp(refractedUV - dispersionUV, vec2(0.0), vec2(1.0)), lod);
    vec3 sampleMid = SampleSceneColorBlurred(refractedUV, lod);
    vec3 sampleShort = SampleSceneColorBlurred(clamp(refractedUV + dispersionUV, vec2(0.0), vec2(1.0)), lod);

    return vec3(
        dot(vec3(0.90, 0.05, 0.05), vec3(sampleLong.r, sampleMid.r, sampleShort.r)),
        dot(vec3(0.05, 0.90, 0.05), vec3(sampleLong.g, sampleMid.g, sampleShort.g)),
        dot(vec3(0.05, 0.05, 0.90), vec3(sampleLong.b, sampleMid.b, sampleShort.b)));
}

float CalculateHPWaterMipLevel(float depth, float scalingFactor, float scatterDensity, float maxBlurLevel) {
    float effectiveScale =
        scalingFactor +
        scatterDensity * clamp(u_ForwardScatterBlurDensity, 0.0, 4.0) *
            HPWATER_FORWARD_SCATTER_BLUR_DENSITY_SCALE;
    float mipLevel = log2(1.0 + max(depth, 0.0) * max(effectiveScale, 0.0));
    return clamp(mipLevel, 0.0, maxBlurLevel);
}

vec2 FindRefractedUV(vec2 uv,
                     vec3 waterWorldPos,
                     vec3 sceneWorldPos,
                     vec3 waterNormal,
                     float waterLinearDepth,
                     float sceneLinearDepth) {
    vec3 refractDir = ComputeHPWaterRefractionDirection(waterWorldPos, sceneWorldPos, waterNormal);
    if (dot(refractDir, refractDir) <= 0.000001) {
        return uv;
    }

    float maxCrossDistance = clamp(u_MaxRefractionCrossDistance, 0.1, 200.0);
    vec3 startNDC = ProjectWorldToNDCLinear(waterWorldPos);
    vec3 endNDC = ProjectWorldToNDCLinear(waterWorldPos + refractDir * maxCrossDistance);
    if (any(lessThan(startNDC.xy, vec2(0.0))) || any(greaterThan(startNDC.xy, vec2(1.0))) ||
        endNDC.z < 0.0) {
        return uv;
    }

    vec3 ndcDir = endNDC - startNDC;
    float maxTravel = length(ndcDir.xy);
    if (maxTravel <= 0.00001) {
        return uv;
    }

    float fallbackDistance = max(length(sceneWorldPos - waterWorldPos), 0.05);
    vec3 fallbackNDC = ProjectWorldToNDCLinear(waterWorldPos + refractDir * fallbackDistance);
    bool fallbackValid =
        !any(lessThan(fallbackNDC.xy, vec2(0.0))) &&
        !any(greaterThan(fallbackNDC.xy, vec2(1.0))) &&
        fallbackNDC.z >= 0.0;
    vec3 hitNDC = fallbackValid ? fallbackNDC : vec3(uv, sceneLinearDepth);

    const float expFactor = 8.0;
    int sampleCount = clamp(u_RefractionSampleCount, 4, 64);
    float hitTolerance = clamp(u_RefractionThicknessOffset, 0.01, 8.0);
    float previousD = 0.0;
    float dither = RefractionStepJitter(uv);
    float invSampleCount = 1.0 / float(sampleCount);
    bool hit = false;

    for (int i = 1; i <= sampleCount; ++i) {
        float linearStep = (float(i - 1) + dither) * invSampleCount;
        float d = (pow(expFactor, linearStep) - 1.0) / (expFactor - 1.0);
        if (i == sampleCount) {
            d = 1.0;
        }

        vec3 sampleNDC = startNDC + ndcDir * d;
        if (any(lessThan(sampleNDC.xy, vec2(0.0))) || any(greaterThan(sampleNDC.xy, vec2(1.0)))) {
            break;
        }

        vec2 sampleUV = clamp(sampleNDC.xy, vec2(0.001), vec2(0.999));
        float sampleDepth = SampleSceneDepth(sampleUV, DepthPyramidLOD(d, maxTravel));
        if (sampleDepth >= 0.9999) {
            previousD = d;
            continue;
        }

        float sampleLinear = LinearizeDepth(sampleDepth);
        float rayLinear = sampleNDC.z;
        if (sampleLinear <= waterLinearDepth + 0.02) {
            previousD = d;
            continue;
        }

        if (abs(rayLinear - sampleLinear) <= hitTolerance || rayLinear >= sampleLinear) {
            float lo = previousD;
            float hi = d;
            for (int refine = 0; refine < 5; ++refine) {
                float mid = (lo + hi) * 0.5;
                vec3 refineNDC = startNDC + ndcDir * mid;
                vec2 refineUV = clamp(refineNDC.xy, vec2(0.001), vec2(0.999));
                float fineDepth = SampleSceneDepth(refineUV, 0.0);
                float fineLinear = LinearizeDepth(fineDepth);
                if (fineDepth < 0.9999 &&
                    fineLinear > waterLinearDepth + 0.02 &&
                    (abs(refineNDC.z - fineLinear) <= hitTolerance || refineNDC.z >= fineLinear)) {
                    hi = mid;
                } else {
                    lo = mid;
                }
            }
            hitNDC = startNDC + ndcDir * hi;
            hit = true;
            break;
        }

        previousD = d;
    }

    if (!hit && sceneLinearDepth <= waterLinearDepth + 0.02) {
        return uv;
    }

    vec2 uvOffset = clamp(hitNDC.xy, vec2(0.001), vec2(0.999)) - uv;
    float edgeFade = pow(ScreenEdgeFade(uv), 0.5);
    return clamp(uv + uvOffset * edgeFade, vec2(0.001), vec2(0.999));
}

struct VolumeSample {
    vec3 color;
    vec3 transmittance;
    float weight;
};

VolumeSample SampleHPWaterVolume(vec2 uv, float sceneLinearDepth) {
    if (u_HPWaterVolumeFullResolution == 1) {
        vec4 volumeColor = texture(u_HPWaterVolumeColor, uv);
        vec4 transmittance = texture(u_HPWaterVolumeTransmittance, uv);
        vec4 volumeDepth = texture(u_HPWaterVolumeDepth, uv);
        float depthWeight = 1.0 / (abs(volumeDepth.r - sceneLinearDepth) + 0.18);
        float validWeight = step(0.001, volumeColor.a + transmittance.a + volumeDepth.a);

        VolumeSample result;
        result.color = validWeight > 0.0 ? volumeColor.rgb : vec3(0.0);
        result.transmittance = validWeight > 0.0
            ? clamp(transmittance.rgb, vec3(0.0), vec3(1.0))
            : vec3(1.0);
        result.weight = validWeight > 0.0 ? clamp(depthWeight, 0.0, 1.0) : 0.0;
        return result;
    }

    ivec2 volumeSize = textureSize(u_HPWaterVolumeColor, 0);
    vec2 volumeTexel = 1.0 / vec2(max(volumeSize, ivec2(1)));
    vec2 volumePixel = uv * vec2(volumeSize) - vec2(0.5);
    ivec2 basePixel = ivec2(floor(volumePixel));
    vec2 fracPixel = fract(volumePixel);

    vec3 colorAccum = vec3(0.0);
    vec3 transAccum = vec3(0.0);
    float totalWeight = 0.0;

    for (int y = 0; y <= 1; ++y) {
        for (int x = 0; x <= 1; ++x) {
            ivec2 p = clamp(basePixel + ivec2(x, y), ivec2(0), volumeSize - ivec2(1));
            vec2 sampleUV = (vec2(p) + vec2(0.5)) * volumeTexel;
            vec4 volumeColor = texture(u_HPWaterVolumeColor, sampleUV);
            vec4 transmittance = texture(u_HPWaterVolumeTransmittance, sampleUV);
            vec4 volumeDepth = texture(u_HPWaterVolumeDepth, sampleUV);

            float bilinearWeight = ((x == 0) ? (1.0 - fracPixel.x) : fracPixel.x) *
                ((y == 0) ? (1.0 - fracPixel.y) : fracPixel.y);
            float depthWeight = 1.0 / (abs(volumeDepth.r - sceneLinearDepth) + 0.18);
            float validWeight = step(0.001, volumeColor.a + transmittance.a + volumeDepth.a);
            float w = bilinearWeight * depthWeight * validWeight;

            colorAccum += volumeColor.rgb * w;
            transAccum += transmittance.rgb * w;
            totalWeight += w;
        }
    }

    VolumeSample result;
    if (totalWeight > 0.00001) {
        result.color = colorAccum / totalWeight;
        result.transmittance = clamp(transAccum / totalWeight, vec3(0.0), vec3(1.0));
        result.weight = clamp(totalWeight, 0.0, 1.0);
    } else {
        result.color = vec3(0.0);
        result.transmittance = vec3(1.0);
        result.weight = 0.0;
    }
    return result;
}

void main() {
    vec4 sceneColor = texture(u_SceneColor, v_UV);
    float waterDepth = texture(u_HPWaterDepth, v_UV).r;
    float waterMask = u_HPWaterMaskEnabled == 1
        ? texture(u_HPWaterMask, v_UV).r
        : (waterDepth < 0.9999 ? 1.0 : 0.0);

    if (waterMask < 0.5 || waterDepth >= 0.9999) {
        FragColor = sceneColor;
        RefractData = vec4(0.0);
        RefractMeta = vec4(v_UV, 1.0, 0.0);
        SSRDiagnostics = vec4(0.0);
        return;
    }

    float sceneDepth = texture(u_SceneDepth, v_UV).r;

    // Preserve foreground opaque objects. GL depth is smaller when closer.
    if (sceneDepth < waterDepth - 0.00005) {
        FragColor = sceneColor;
        RefractData = vec4(0.0);
        RefractMeta = vec4(v_UV, sceneDepth, 0.0);
        SSRDiagnostics = vec4(0.0);
        return;
    }

    vec4 normalRoughness = texture(u_HPWaterNormalRoughness, v_UV);
    vec4 scatterThickness = texture(u_HPWaterScatterThickness, v_UV);
    vec4 absorptionFoam = texture(u_HPWaterAbsorptionFoam, v_UV);

    vec3 N = normalize(normalRoughness.xyz * 2.0 - 1.0);
    float roughness = clamp(normalRoughness.a, 0.02, 0.85);
    vec3 scatterColor = max(scatterThickness.rgb, vec3(0.0));
    float depthTintDistance = max(scatterThickness.a, 0.1);
    vec3 absorptionColor = max(absorptionFoam.rgb, vec3(0.0001));
    float foam = clamp(absorptionFoam.a, 0.0, 1.0);

    float waterLinear = LinearizeDepth(waterDepth);
    float sceneLinear = sceneDepth >= 0.9999
        ? waterLinear + depthTintDistance
        : LinearizeDepth(sceneDepth);
    float thickness = max(sceneLinear - waterLinear, 0.0);
    float normalizedThickness = clamp(thickness / depthTintDistance, 0.0, 1.0);

    vec3 waterWorldPos = ReconstructWorldPosition(v_UV, waterDepth);
    vec3 sceneWorldPos = sceneDepth >= 0.9999
        ? waterWorldPos + NormalizeOr(waterWorldPos - u_ViewPos, vec3(0.0, 0.0, 1.0)) * depthTintDistance
        : ReconstructWorldPosition(v_UV, sceneDepth);

    // HPWater-style refraction slice: compute a world-space refracted ray,
    // project it into NDC, then march the 3D NDC line against the opaque
    // scene-depth pyramid with exponential steps and mip-0 refinement.
    vec2 refractUV = FindRefractedUV(v_UV, waterWorldPos, sceneWorldPos, N, waterLinear, sceneLinear);

    float refractedSceneDepth = SampleSceneDepth(refractUV, 0.0);
    if (refractedSceneDepth < waterDepth - 0.00005) {
        refractUV = v_UV;
        refractedSceneDepth = sceneDepth;
    }

    vec3 refractedColor = texture(u_SceneColor, refractUV).rgb;
    float worldDepth = refractedSceneDepth >= 0.9999 ? waterDepth : refractedSceneDepth;
    vec3 refractedWorldPos = ReconstructWorldPosition(refractUV, worldDepth);
    float rayLength = length(refractedWorldPos - waterWorldPos);
    vec3 fallbackTransmittance = exp(-absorptionColor * (0.35 + normalizedThickness * 2.35));
    vec3 fallbackBodyColor = refractedColor * fallbackTransmittance +
        scatterColor * (vec3(1.0) - fallbackTransmittance) * (0.45 + 0.35 * normalizedThickness);

    vec3 bodyColor = fallbackBodyColor;
    if (u_HPWaterVolumeEnabled == 1) {
        float refractedLinearDepth = refractedSceneDepth >= 0.9999
            ? waterLinear + depthTintDistance
            : LinearizeDepth(refractedSceneDepth);
        VolumeSample volume = SampleHPWaterVolume(v_UV, refractedLinearDepth);
        vec3 volumeBody = refractedColor * volume.transmittance + volume.color;
        bodyColor = mix(fallbackBodyColor, volumeBody, volume.weight);
    }

    vec3 viewDelta = u_ViewPos - waterWorldPos;
    vec3 V = length(viewDelta) > 0.0001 ? normalize(viewDelta) : vec3(0.0, 1.0, 0.0);
    vec3 L = length(u_LightDir) > 0.0001 ? normalize(u_LightDir) : normalize(vec3(-0.35, 0.82, 0.44));
    vec3 H = length(V + L) > 0.0001 ? normalize(V + L) : N;
    float NdotV = clamp(dot(N, V), 0.0, 1.0);
    float NdotLRaw = dot(N, L);
    float NdotL = clamp(NdotLRaw, 0.0, 1.0);
    float lightViewAlignment = clamp(dot(-V, L), -1.0, 1.0);
    float backlit = pow(clamp(lightViewAlignment * 0.5 + 0.5, 0.0, 1.0), 1.5) *
        smoothstep(0.0, 0.7, 1.0 - NdotL);
    vec3 F0 = vec3(HPWATER_WATER_F0);
    vec3 F = FresnelSchlickRoughness(NdotV, F0, roughness);
    vec3 Fgd = ApplyFGD(F, F0, roughness, NdotV);
    float energyCompensation = GGXEnergyCompensation(F0, roughness);
    float D = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    float directionalShadow = ComputeShadow(waterWorldPos, N, 0.0);
    vec3 directSpecular = (D * G * F) / max(4.0 * NdotV * NdotL, 0.001);
    float directionalSpecularSelfOcclusion = HPWaterSpecularSelfOcclusion(NdotL);
    directSpecular *= u_LightColor * max(u_LightIntensity, 0.0) *
        NdotL * directionalShadow * directionalSpecularSelfOcclusion * energyCompensation;
    vec3 punctualMacroLight = vec3(0.0);
    vec3 punctualThinSSSLight = vec3(0.0);
    vec3 punctualBacklitLight = vec3(0.0);

    int pointCount = clamp(u_NumPointLights, 0, 8);
    for (int i = 0; i < 8; ++i) {
        if (i >= pointCount) {
            break;
        }

        vec3 lightVector = u_PointLightPositions[i] - waterWorldPos;
        float lightDistance = length(lightVector);
        float lightRange = max(u_PointLightRanges[i], 0.001);
        if (lightDistance <= 0.0001 || lightDistance >= lightRange) {
            continue;
        }

        vec3 localL = lightVector / lightDistance;
        float range01 = clamp(lightDistance / lightRange, 0.0, 1.0);
        float rangeWindow = 1.0 - pow(range01, 4.0);
        rangeWindow *= rangeWindow;
        float attenuation = rangeWindow / (lightDistance * lightDistance + 1.0);
        vec3 radiance = u_PointLightColors[i] * max(u_PointLightIntensities[i], 0.0) * attenuation;
        directSpecular += EvaluateHPWaterSpecularLight(
            N, V, localL, radiance, roughness, F, energyCompensation);
        float localNdotLRaw = dot(N, localL);
        float localNdotL = clamp(localNdotLRaw, 0.0, 1.0);
        float localTEntry = 1.0 - SchlickFresnel(localNdotL, HPWATER_WATER_F0);
        punctualMacroLight += radiance * localNdotL * localTEntry;
        punctualThinSSSLight += radiance * (1.0 - localNdotL);
        punctualBacklitLight += radiance * clamp(-localNdotLRaw, 0.0, 1.0);
    }

    int spotCount = clamp(u_NumSpotLights, 0, 4);
    for (int i = 0; i < 4; ++i) {
        if (i >= spotCount) {
            break;
        }

        vec3 lightVector = u_SpotLightPositions[i] - waterWorldPos;
        float lightDistance = length(lightVector);
        float lightRange = max(u_SpotLightRanges[i], 0.001);
        if (lightDistance <= 0.0001 || lightDistance >= lightRange) {
            continue;
        }

        vec3 localL = lightVector / lightDistance;
        vec3 spotForward = NormalizeOr(-u_SpotLightDirections[i], vec3(0.0, -1.0, 0.0));
        float theta = dot(localL, spotForward);
        float coneWidth = max(u_SpotLightInnerCos[i] - u_SpotLightOuterCos[i], 0.0001);
        float spotFactor = clamp((theta - u_SpotLightOuterCos[i]) / coneWidth, 0.0, 1.0);
        if (spotFactor <= 0.0001) {
            continue;
        }

        float range01 = clamp(lightDistance / lightRange, 0.0, 1.0);
        float rangeWindow = 1.0 - pow(range01, 4.0);
        rangeWindow *= rangeWindow;
        float attenuation = rangeWindow * spotFactor / (lightDistance * lightDistance + 1.0);
        vec3 radiance = u_SpotLightColors[i] * max(u_SpotLightIntensities[i], 0.0) * attenuation;
        directSpecular += EvaluateHPWaterSpecularLight(
            N, V, localL, radiance, roughness, F, energyCompensation);
        float localNdotLRaw = dot(N, localL);
        float localNdotL = clamp(localNdotLRaw, 0.0, 1.0);
        float localTEntry = 1.0 - SchlickFresnel(localNdotL, HPWATER_WATER_F0);
        punctualMacroLight += radiance * localNdotL * localTEntry;
        punctualThinSSSLight += radiance * (1.0 - localNdotL);
        punctualBacklitLight += radiance * clamp(-localNdotLRaw, 0.0, 1.0);
    }

    int areaCount = clamp(u_NumAreaLights, 0, 4);
    for (int i = 0; i < 4; ++i) {
        if (i >= areaCount) {
            break;
        }

        vec3 localL = vec3(0.0, 1.0, 0.0);
        float areaDiffuseScale = 1.0;
        vec3 radiance = ComputeHPWaterAreaLightRadiance(waterWorldPos,
                                                        N,
                                                        V,
                                                        roughness,
                                                        i,
                                                        localL,
                                                        areaDiffuseScale);
        if (max(max(radiance.r, radiance.g), radiance.b) <= 0.00001) {
            continue;
        }

        directSpecular += EvaluateHPWaterSpecularLight(
            N, V, localL, radiance, roughness, F, energyCompensation);
        float localNdotLRaw = dot(N, localL);
        float localNdotL = clamp(localNdotLRaw, 0.0, 1.0);
        float localTEntry = 1.0 - SchlickFresnel(localNdotL, HPWATER_WATER_F0);
        vec3 areaBodyRadiance = radiance * areaDiffuseScale;
        punctualMacroLight += areaBodyRadiance * localNdotL * localTEntry;
        punctualThinSSSLight += areaBodyRadiance * (1.0 - localNdotL);
        punctualBacklitLight += areaBodyRadiance * clamp(-localNdotLRaw, 0.0, 1.0);
    }

    vec3 skyReflection = vec3(0.0);
    vec3 indirectBody = vec3(0.0);
    float ssrConfidence = 0.0;
    float ssrHit = 0.0;
    float probeHierarchyWeight = 0.0;
    float skyHierarchyWeight = 0.0;
    if (u_IndirectLightingEnabled == 1) {
        vec3 R = reflect(-V, N);
        float roughnessFade = mix(1.0, 0.25, roughness);
        vec4 ssrReflection = u_HPWaterSSRLightingEnabled == 1
            ? texture(u_HPWaterSSRLighting, v_UV)
            : vec4(0.0);
        ssrConfidence = clamp(ssrReflection.a, 0.0, 1.0);
        ssrHit = ssrConfidence > 0.0001 ? 1.0 : 0.0;
        vec3 environmentSpecular =
            ssrReflection.rgb +
            SampleHPWaterSpecularEnvironmentHierarchy(
                R, waterWorldPos, N, V, roughness, ssrConfidence,
                probeHierarchyWeight, skyHierarchyWeight);
        vec3 environmentDiffuse = SampleEnvironment(N, N, waterWorldPos, N, V, 1.0, true);
        float environmentIntensity =
            ssrConfidence +
            probeHierarchyWeight * clamp(u_ReflectionProbeIntensity, 0.0, 4.0) +
            skyHierarchyWeight * clamp(u_SkyReflectionIntensity, 0.0, 4.0);
        skyReflection = environmentSpecular * Fgd *
            (0.35 + environmentIntensity * 2.35) *
            clamp(u_EnvironmentReflectionIntensity, 0.0, 3.0) *
            roughnessFade *
            energyCompensation;
        float indirectDither = InterleavedGradientNoise(v_UV * vec2(textureSize(u_SceneColor, 0)),
                                                        u_FrameIndex);
        vec3 indirectLighting = environmentDiffuse *
            clamp(u_IndirectDiffuseIntensity, 0.0, 4.0) *
            clamp(u_IndirectLightStrength, 0.0, 4.0);
        indirectBody = AccumulateHPWaterIndirectScattering(
            indirectLighting,
            absorptionColor,
            scatterColor,
            min(rayLength, clamp(u_MaxRefractionCrossDistance, 0.1, 200.0)),
            max(normalizedThickness * 2.0, 0.0),
            indirectDither);
    }
    vec3 reflected = skyReflection + directSpecular * clamp(u_EnvironmentReflectionIntensity, 0.0, 3.0);
    float forwardPhase = HenyeyGreenstein(lightViewAlignment, clamp(u_PhaseG, -0.95, 0.95));
    float forwardStrength = clamp(u_ForwardScatterStrength, 0.0, 3.0);
    float scatterDensity = clamp(dot(scatterColor, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
    float hpWaterRayLength = min(rayLength, clamp(u_MaxRefractionCrossDistance, 0.1, 200.0));
    float forwardBlurLOD = max(
        2.0,
        CalculateHPWaterMipLevel(
            hpWaterRayLength,
            HPWATER_FORWARD_SCALING_FACTOR,
            scatterDensity,
            6.0));
    vec3 forwardBlur = SampleHPWaterDispersedSceneColor(v_UV, refractUV, forwardBlurLOD);

    // HPWaterBSDFLibary component split:
    // diffR = T_entry * G_entry * S_volume
    // diffT = thinLayerSSS + backlitTransmission
    vec3 extinctionCoeff = max(absorptionColor + scatterColor, vec3(0.00001));
    vec3 T_entry = vec3(1.0) - vec3(SchlickFresnel(NdotL, HPWATER_WATER_F0));
    float G_entry = NdotL;
    vec3 forwardScatterColor = forwardBlur * clamp(u_MultiScatterScale, 0.0, 32.0);
    vec3 volumePhase = HPWaterScatterPhase(lightViewAlignment, clamp(u_PhaseG, -0.95, 0.95));
    vec3 directionalWaterLight = u_LightColor * max(u_LightIntensity, 0.0) * directionalShadow;
    vec3 macroLight = directionalWaterLight * T_entry * G_entry + punctualMacroLight;
    vec3 S_volume = HPWaterScatteredLight(
        macroLight + forwardScatterColor * 0.22,
        absorptionColor,
        scatterColor,
        max(hpWaterRayLength, 0.01),
        mix(vec3(forwardPhase), volumePhase, 0.65));
    S_volume *= (0.15 + 0.85 * normalizedThickness) * forwardStrength;
    vec3 macroScattering = S_volume;

    float sssThickness = max(normalizedThickness, 0.001);
    float scatterStrength = dot(scatterColor, vec3(0.2126, 0.7152, 0.0722));
    float L_linear = sssThickness * HPWATER_SSS_PATH_SCALE;
    float L_nonlinear = sssThickness * sssThickness * HPWATER_SSS_PATH_SCALE * (1.0 + scatterStrength);
    float opticalDepth = dot(extinctionCoeff, vec3(0.2126, 0.7152, 0.0722)) *
        sssThickness * HPWATER_SSS_PATH_SCALE;
    float nonlinearWeight = clamp(opticalDepth * HPWATER_SSS_NONLINEAR_STRENGTH, 0.0, 1.0);
    float sssPathLength = mix(L_linear, L_nonlinear, nonlinearWeight);
    vec3 sssTransmittance;
    float G_sss = 1.0 - G_entry;
    vec3 thinSSSLight = directionalWaterLight * G_sss + punctualThinSSSLight;
    vec3 S_sss = HPWaterScatteredLightWithTransmittance(
        thinSSSLight,
        absorptionColor,
        scatterColor,
        sssPathLength,
        HPWaterScatterPhase(dot(-V, L), clamp(u_PhaseG, -0.95, 0.95)),
        sssTransmittance);
    float sssWeight = clamp(1.0 - dot(sssTransmittance, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
    vec3 thinLayerSSS = S_sss * HPWATER_SSS_SCATTER_BOOST *
        clamp(u_ThinSSSStrength, 0.0, 3.0);
    thinLayerSSS = mix(S_volume * G_sss, thinLayerSSS, sssWeight);

    float G_backlit = clamp(-NdotLRaw, 0.0, 1.0);
    float backlitPathLength = sssThickness * HPWATER_BACKLIT_PATH_SCALE;
    vec3 T_backlit = exp(-extinctionCoeff * backlitPathLength);
    float P_backlit = HPWaterHenyeyPhase(dot(V, -L), 0.9998);
    vec3 backlitLight = directionalWaterLight * G_backlit + punctualBacklitLight;
    vec3 backlitTransmission = backlitLight * T_backlit * P_backlit *
        clamp(u_BacklitTransmissionStrength, 0.0, 3.0);

    bodyColor += macroScattering + thinLayerSSS + backlitTransmission + indirectBody;

    if (u_HPWaterCausticEnabled == 1) {
        vec4 caustic = texture(u_HPWaterCaustic, v_UV);
        float receiverWeight = (0.25 + 0.75 * normalizedThickness) * (1.0 - foam * 0.55);
        bodyColor += caustic.rgb * receiverWeight;
    }

    vec3 T_exit = vec3(1.0) - vec3(SchlickFresnel(NdotV, HPWATER_WATER_F0));
    vec3 exitedBodyColor = bodyColor * T_exit;

    vec3 foamColor = mix(vec3(0.88, 0.94, 0.98), vec3(1.0), foam);
    vec3 waterColor = mix(exitedBodyColor + reflected, foamColor, foam * 0.65);
    float waterAlpha = clamp(0.28 + normalizedThickness * 0.62 + foam * 0.45, 0.0, 0.92);

    FragColor = vec4(mix(sceneColor.rgb, waterColor, waterAlpha), sceneColor.a);
    RefractData = vec4(refractedWorldPos, rayLength);
    RefractMeta = vec4(refractUV, refractedSceneDepth, normalizedThickness);
    SSRDiagnostics = vec4(ssrConfidence,
                          ssrHit,
                          probeHierarchyWeight,
                          skyHierarchyWeight);
}
#endif

            ENDGLSL
        }
    }

    FallBack "VibeEngine/DeferredLighting"
}
