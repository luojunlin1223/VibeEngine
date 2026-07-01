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
layout(location = 4) out vec4 AreaLightDiagnostics;
layout(location = 5) out vec4 ForwardScatterDiagnostics;
layout(location = 6) out vec4 PunctualLightDiagnostics;
layout(location = 7) out vec4 LocalLightShadowDiagnostics;

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
uniform vec3 u_HPFoamColor;
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
uniform int u_HPWaterTiledLightListEnabled;
uniform int u_HPWaterTiledLightListTileSize;
uniform int u_HPWaterTiledLightListGridWidth;
uniform int u_HPWaterTiledLightListGridHeight;
uniform int u_HPWaterTiledLightListTileMinX;
uniform int u_HPWaterTiledLightListTileMinY;
uniform int u_HPWaterTiledLightListTileRectWidth;
uniform int u_HPWaterTiledLightListTileRectHeight;
uniform int u_HPWaterTiledLightListTileHeaderCount;
uniform int u_HPWaterTiledLightListReferenceOffset;
uniform int u_HPWaterTiledLightListReferenceCount;
uniform int u_HPWaterLightPayloadEnabled;
uniform int u_HPWaterPointLightPayloadCount;
uniform int u_HPWaterSpotLightPayloadCount;
uniform int u_HPWaterAreaLightPayloadCount;
uniform vec3 u_IndirectSkyColor;
uniform vec3 u_IndirectGroundColor;
uniform vec3 u_IndirectTint;
uniform int u_IndirectLightingEnabled;
uniform float u_IndirectDiffuseIntensity;
uniform float u_SkyReflectionIntensity;
uniform float u_ReflectionProbeIntensity;
uniform float u_ReflectionProbeBlend;
uniform float u_ReflectionProbeHierarchyWeight;
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
uniform int u_FrameIndex;
uniform mat4 u_ViewProjection;
uniform mat4 u_InverseViewProjection;

#include "shadows.glslinc"
#include "hpwater_common.glslinc"
#include "hpwater_normal.glslinc"

const float PI = 3.14159265358979323846;
const float HPWATER_FORWARD_SCALING_FACTOR = 1.0;
const float HPWATER_WATER_DISPERSION_UV_CLAMP = 0.01;
const int HPWATER_INDIRECT_SAMPLE_COUNT = 16;
const float HPWATER_INDIRECT_EXP_FACTOR = 32.0;
const float HPWATER_SSS_PATH_SCALE = 20.0;
const float HPWATER_SSS_NONLINEAR_STRENGTH = 0.5;
const float HPWATER_SSS_SCATTER_BOOST = 2.0;
const float HPWATER_BACKLIT_PATH_SCALE = 20.0;
const float HPWATER_WATER_F0 = 0.02037;
const int HPWATER_MAX_TILE_LIGHT_REFERENCES = 64;
const uint HPWATER_LIGHT_REF_SPOT_FLAG = 0x00010000u;
const uint HPWATER_LIGHT_REF_AREA_FLAG = 0x80000000u;

layout(std430, binding = 7) readonly buffer HPWaterTiledLightListBuffer {
    uint u_HPWaterTiledLightListData[];
};
layout(std430, binding = 8) readonly buffer HPWaterPointLightPayloadBuffer {
    vec4 u_HPWaterPointLightPayload[];
};
layout(std430, binding = 9) readonly buffer HPWaterSpotLightPayloadBuffer {
    vec4 u_HPWaterSpotLightPayload[];
};
layout(std430, binding = 10) readonly buffer HPWaterAreaLightPayloadBuffer {
    vec4 u_HPWaterAreaLightPayload[];
};

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

float FiniteOr(float v, float fallback) {
    return (isnan(v) || isinf(v)) ? fallback : v;
}

vec3 FiniteOr(vec3 v, vec3 fallback) {
    return vec3(
        FiniteOr(v.x, fallback.x),
        FiniteOr(v.y, fallback.y),
        FiniteOr(v.z, fallback.z));
}

float Luminance(vec3 color) {
    return dot(max(color, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722));
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

vec4 ProjectWorldToScreenDepth(vec3 worldPos) {
    vec4 clip = u_ViewProjection * vec4(worldPos, 1.0);
    if (clip.w <= 0.00001) {
        return vec4(-1.0);
    }

    vec3 ndc = clip.xyz / clip.w;
    return vec4(ndc.xy * 0.5 + 0.5, ndc.z * 0.5 + 0.5, 1.0);
}

float ComputeHPWaterLocalLightScreenShadow(vec3 receiverWS, vec3 lightWS) {
    vec3 ray = lightWS - receiverWS;
    float rayLength = length(ray);
    if (rayLength <= 0.05) {
        return 1.0;
    }

    vec3 direction = ray / rayLength;
    float occlusion = 0.0;
    const int sampleCount = 14;
    for (int sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
        float t = (float(sampleIndex) + 0.65) / float(sampleCount);
        float distanceAlongRay = mix(0.08, rayLength - 0.08, t);
        if (distanceAlongRay <= 0.0 || distanceAlongRay >= rayLength) {
            continue;
        }

        vec3 sampleWS = receiverWS + direction * distanceAlongRay;
        vec4 sampleScreen = ProjectWorldToScreenDepth(sampleWS);
        if (sampleScreen.w <= 0.0 ||
            sampleScreen.x <= 0.001 || sampleScreen.x >= 0.999 ||
            sampleScreen.y <= 0.001 || sampleScreen.y >= 0.999 ||
            sampleScreen.z <= 0.0 || sampleScreen.z >= 1.0) {
            continue;
        }

        float sceneDepth = texture(u_SceneDepth, sampleScreen.xy).r;
        if (sceneDepth >= 0.9999 || sceneDepth <= 0.00001) {
            continue;
        }

        float sceneLinear = LinearizeDepth(sceneDepth);
        float sampleLinear = LinearizeDepth(sampleScreen.z);
        float depthDelta = sampleLinear - sceneLinear;
        float rayWindow = clamp((rayLength - distanceAlongRay) / max(rayLength, 0.001), 0.0, 1.0);
        float bias = mix(0.08, 0.28, t);
        if (depthDelta > bias) {
            float rawDepthDelta = sampleScreen.z - sceneDepth;
            float depthWeight = smoothstep(bias, bias + 0.75, depthDelta);
            float rawWeight = smoothstep(0.00005, 0.0025, rawDepthDelta);
            occlusion = max(occlusion, depthWeight * rawWeight * rayWindow);
        }
    }

    return clamp(1.0 - occlusion * 0.92, 0.08, 1.0);
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

float HPWaterAdaptiveRefractionExpFactor(float distance) {
    return clamp(pow(max(distance, 0.01) / 20.0, 2.0), 1.01, 32.0);
}

float ScreenEdgeFade(vec2 uv) {
    vec2 edge = min(uv, vec2(1.0) - uv);
    return clamp(min(edge.x, edge.y) * 8.0, 0.0, 1.0);
}

float HPWaterRefractionBoundFade(vec2 positionNDC) {
    const vec2 boundScale = vec2(6.0, 6.0);
    float boundX = clamp(positionNDC.x * boundScale.x, 0.0, 1.0) *
        clamp((1.0 - positionNDC.x) * boundScale.x, 0.0, 1.0);
    float boundY = clamp(positionNDC.y * boundScale.y, 0.0, 1.0) *
        clamp((1.0 - positionNDC.y) * boundScale.y, 0.0, 1.0);
    return clamp(pow(boundX * boundY, 0.5), 0.0, 1.0);
}

float InterleavedGradientNoise(vec2 pixelPos, int frameIndex) {
    return HPWaterInterleavedGradientNoise(pixelPos, frameIndex);
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
    if (u_HPWaterLightPayloadEnabled == 1 && lightIndex >= u_HPWaterAreaLightPayloadCount) {
        L = vec3(0.0, 1.0, 0.0);
        diffuseScale = 1.0;
        return vec3(0.0);
    }

    vec3 center = u_AreaLightPositions[lightIndex];
    vec3 right = NormalizeOr(u_AreaLightRights[lightIndex], vec3(1.0, 0.0, 0.0));
    vec3 up = NormalizeOr(u_AreaLightUps[lightIndex], vec3(0.0, 1.0, 0.0));
    vec3 forward = NormalizeOr(u_AreaLightForwards[lightIndex], vec3(0.0, 0.0, 1.0));
    vec3 color = u_AreaLightColors[lightIndex];
    float intensity = u_AreaLightIntensities[lightIndex];
    float width = max(u_AreaLightWidths[lightIndex], 0.001);
    float height = max(u_AreaLightHeights[lightIndex], 0.001);
    float range = max(u_AreaLightRanges[lightIndex], 0.001);
    vec3 fallbackCenter = center;
    vec3 fallbackRight = right;
    vec3 fallbackUp = up;
    vec3 fallbackForward = forward;
    vec3 fallbackColor = color;
    float fallbackIntensity = intensity;
    float fallbackWidth = width;
    float fallbackHeight = height;
    float fallbackRange = range;
    if (u_HPWaterLightPayloadEnabled == 1) {
        int base = lightIndex * 5;
        vec4 positionRange = u_HPWaterAreaLightPayload[base + 0];
        vec4 rightWidth = u_HPWaterAreaLightPayload[base + 1];
        vec4 upHeight = u_HPWaterAreaLightPayload[base + 2];
        vec4 forwardPayload = u_HPWaterAreaLightPayload[base + 3];
        vec4 colorIntensity = u_HPWaterAreaLightPayload[base + 4];
        center = positionRange.xyz;
        range = max(positionRange.w, 0.001);
        right = NormalizeOr(rightWidth.xyz, vec3(1.0, 0.0, 0.0));
        width = max(rightWidth.w, 0.001);
        up = NormalizeOr(upHeight.xyz, vec3(0.0, 1.0, 0.0));
        height = max(upHeight.w, 0.001);
        forward = NormalizeOr(forwardPayload.xyz, vec3(0.0, 0.0, 1.0));
        color = colorIntensity.xyz;
        intensity = colorIntensity.w;
        if (lightIndex < 4 && (range <= 0.001 || intensity <= 0.0 || Luminance(color) <= 0.00001)) {
            center = fallbackCenter;
            right = fallbackRight;
            up = fallbackUp;
            forward = fallbackForward;
            color = fallbackColor;
            intensity = fallbackIntensity;
            width = fallbackWidth;
            height = fallbackHeight;
            range = fallbackRange;
        }
    }
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
    float projectedArea = width * height;
    float solidAngleFallback = clamp(projectedArea /
        max(distanceToLight * distanceToLight + projectedArea, 0.001), 0.0, 1.0);
    specIrradiance = max(specIrradiance, solidAngleFallback * 0.25);
    diffuseIrradiance = max(diffuseIrradiance, solidAngleFallback);

    diffuseScale = clamp(diffuseIrradiance / max(specIrradiance, 0.001), 0.0, 1.5);
    float attenuation = FiniteOr(rangeWindow * emissionFacing * specIrradiance, 0.0);
    return FiniteOr(color, vec3(0.0)) * max(FiniteOr(intensity, 0.0), 0.0) * attenuation;
}

bool HPWaterFetchTiledLightList(vec2 pixelCoord, out int referenceOffset, out int referenceCount) {
    referenceOffset = 0;
    referenceCount = 0;
    if (u_HPWaterTiledLightListEnabled != 1 ||
        u_HPWaterTiledLightListTileSize <= 0 ||
        u_HPWaterTiledLightListTileRectWidth <= 0 ||
        u_HPWaterTiledLightListTileRectHeight <= 0 ||
        u_HPWaterTiledLightListTileHeaderCount <= 0 ||
        u_HPWaterTiledLightListReferenceCount <= 0) {
        return false;
    }

    ivec2 tile = ivec2(floor(pixelCoord / float(u_HPWaterTiledLightListTileSize)));
    ivec2 relTile = tile - ivec2(u_HPWaterTiledLightListTileMinX, u_HPWaterTiledLightListTileMinY);
    if (relTile.x < 0 || relTile.y < 0 ||
        relTile.x >= u_HPWaterTiledLightListTileRectWidth ||
        relTile.y >= u_HPWaterTiledLightListTileRectHeight) {
        return false;
    }

    int headerIndex = relTile.y * u_HPWaterTiledLightListTileRectWidth + relTile.x;
    if (headerIndex < 0 || headerIndex >= u_HPWaterTiledLightListTileHeaderCount) {
        return false;
    }

    int headerBase = headerIndex * 2;
    referenceOffset = int(u_HPWaterTiledLightListData[headerBase]);
    referenceCount = int(u_HPWaterTiledLightListData[headerBase + 1]);
    return referenceOffset >= 0 && referenceCount > 0 &&
        referenceOffset < u_HPWaterTiledLightListReferenceCount;
}

uint HPWaterReadTiledLightReference(int referenceIndex) {
    return u_HPWaterTiledLightListData[u_HPWaterTiledLightListReferenceOffset + referenceIndex];
}

int HPWaterDecodeLightReferenceIndex(uint referenceValue) {
    return int(referenceValue & 0x0000ffffu);
}

void AccumulateHPWaterRadiance(vec3 radiance,
                               vec3 localL,
                               vec3 N,
                               vec3 V,
                               float roughness,
                               vec3 F,
                               float energyCompensation,
                               inout vec3 directSpecular,
                               inout vec3 specularContribution,
                               inout vec3 macroLight,
                               inout vec3 thinSSSLight,
                               inout vec3 backlitLight,
                               inout vec3 macroContribution,
                               inout vec3 thinSSSContribution,
                               inout vec3 backlitContribution) {
    vec3 punctualSpecular = EvaluateHPWaterSpecularLight(
        N, V, localL, radiance, roughness, F, energyCompensation);
    directSpecular += punctualSpecular;
    specularContribution += punctualSpecular;
    float localNdotLRaw = dot(N, localL);
    float localNdotL = clamp(localNdotLRaw, 0.0, 1.0);
    float localTEntry = 1.0 - SchlickFresnel(localNdotL, HPWATER_WATER_F0);
    vec3 macro = radiance * localNdotL * localTEntry;
    vec3 mediumScatter = radiance * 0.08;
    vec3 thinSSS = radiance * max(1.0 - localNdotL, 0.08);
    vec3 backlit = radiance * clamp(-localNdotLRaw, 0.0, 1.0);
    macroLight += macro;
    thinSSSLight += thinSSS + mediumScatter;
    backlitLight += backlit;
    macroContribution += macro;
    thinSSSContribution += thinSSS + mediumScatter;
    backlitContribution += backlit;
}

void AccumulateHPWaterPointLight(int i,
                                 vec3 waterWorldPos,
                                 vec3 N,
                                 vec3 V,
                                 float roughness,
                                 vec3 F,
                                 float energyCompensation,
                                 inout vec3 directSpecular,
                                 inout vec3 specularContribution,
                                 inout vec3 macroLight,
                                 inout vec3 thinSSSLight,
                                 inout vec3 backlitLight,
                                 inout vec3 macroContribution,
                                 inout vec3 thinSSSContribution,
                                 inout vec3 backlitContribution,
                                 inout float localShadowBlocked,
                                 inout float localShadowPath,
                                 inout float rawRadianceEvidence) {
    if (u_HPWaterLightPayloadEnabled == 1 && i >= u_HPWaterPointLightPayloadCount) {
        return;
    }

    vec3 position = u_PointLightPositions[i];
    vec3 color = u_PointLightColors[i];
    float intensity = u_PointLightIntensities[i];
    float range = u_PointLightRanges[i];
    vec3 fallbackPosition = position;
    vec3 fallbackColor = color;
    float fallbackIntensity = intensity;
    float fallbackRange = range;
    if (u_HPWaterLightPayloadEnabled == 1) {
        int base = i * 2;
        vec4 positionRange = u_HPWaterPointLightPayload[base + 0];
        vec4 colorIntensity = u_HPWaterPointLightPayload[base + 1];
        position = positionRange.xyz;
        range = positionRange.w;
        color = colorIntensity.xyz;
        intensity = colorIntensity.w;
        if (i < 8 && (range <= 0.001 || intensity <= 0.0 || Luminance(color) <= 0.00001)) {
            position = fallbackPosition;
            range = fallbackRange;
            color = fallbackColor;
            intensity = fallbackIntensity;
        }
    }

    vec3 lightVector = position - waterWorldPos;
    float lightDistance = length(lightVector);
    float lightRange = max(range, 0.001);
    if (lightDistance <= 0.0001 || lightDistance >= lightRange) {
        return;
    }

    vec3 localL = lightVector / lightDistance;
    float range01 = clamp(lightDistance / lightRange, 0.0, 1.0);
    float rangeWindow = 1.0 - pow(range01, 4.0);
    rangeWindow *= rangeWindow;
    float attenuation = FiniteOr(rangeWindow / (lightDistance * lightDistance + 1.0), 0.0);
    float localShadow = clamp(FiniteOr(ComputeHPWaterLocalLightScreenShadow(waterWorldPos, position), 1.0), 0.08, 1.0);
    vec3 radiance = FiniteOr(color, vec3(0.0)) * max(FiniteOr(intensity, 0.0), 0.0) *
        attenuation * localShadow;
    radiance = FiniteOr(radiance, vec3(0.0));
    rawRadianceEvidence = max(rawRadianceEvidence, Luminance(radiance));
    localShadowBlocked = max(localShadowBlocked, 1.0 - localShadow);
    localShadowPath = max(localShadowPath, 1.0);
    AccumulateHPWaterRadiance(radiance, localL, N, V, roughness, F, energyCompensation,
                              directSpecular, specularContribution,
                              macroLight, thinSSSLight, backlitLight,
                              macroContribution, thinSSSContribution, backlitContribution);
}

void AccumulateHPWaterSpotLight(int i,
                                vec3 waterWorldPos,
                                vec3 N,
                                vec3 V,
                                float roughness,
                                vec3 F,
                                float energyCompensation,
                                inout vec3 directSpecular,
                                inout vec3 specularContribution,
                                inout vec3 macroLight,
                                inout vec3 thinSSSLight,
                                 inout vec3 backlitLight,
                                 inout vec3 macroContribution,
                                 inout vec3 thinSSSContribution,
                                 inout vec3 backlitContribution,
                                 inout float localShadowBlocked,
                                 inout float localShadowPath,
                                 inout float rawRadianceEvidence) {
    if (u_HPWaterLightPayloadEnabled == 1 && i >= u_HPWaterSpotLightPayloadCount) {
        return;
    }

    vec3 position = u_SpotLightPositions[i];
    vec3 direction = u_SpotLightDirections[i];
    vec3 color = u_SpotLightColors[i];
    float intensity = u_SpotLightIntensities[i];
    float range = u_SpotLightRanges[i];
    float innerCos = u_SpotLightInnerCos[i];
    float outerCos = u_SpotLightOuterCos[i];
    if (u_HPWaterLightPayloadEnabled == 1) {
        int base = i * 4;
        vec4 positionRange = u_HPWaterSpotLightPayload[base + 0];
        vec4 directionInner = u_HPWaterSpotLightPayload[base + 1];
        vec4 colorIntensity = u_HPWaterSpotLightPayload[base + 2];
        vec4 outerPayload = u_HPWaterSpotLightPayload[base + 3];
        position = positionRange.xyz;
        range = positionRange.w;
        direction = directionInner.xyz;
        innerCos = directionInner.w;
        color = colorIntensity.xyz;
        intensity = colorIntensity.w;
        outerCos = outerPayload.x;
    }

    vec3 lightVector = position - waterWorldPos;
    float lightDistance = length(lightVector);
    float lightRange = max(range, 0.001);
    if (lightDistance <= 0.0001 || lightDistance >= lightRange) {
        return;
    }

    vec3 localL = lightVector / lightDistance;
    vec3 spotForward = NormalizeOr(-direction, vec3(0.0, -1.0, 0.0));
    float theta = dot(localL, spotForward);
    float coneWidth = max(innerCos - outerCos, 0.0001);
    float spotFactor = clamp((theta - outerCos) / coneWidth, 0.0, 1.0);
    if (spotFactor <= 0.0001) {
        return;
    }

    float range01 = clamp(lightDistance / lightRange, 0.0, 1.0);
    float rangeWindow = 1.0 - pow(range01, 4.0);
    rangeWindow *= rangeWindow;
    float attenuation = FiniteOr(rangeWindow * spotFactor / (lightDistance * lightDistance + 1.0), 0.0);
    float localShadow = clamp(FiniteOr(ComputeHPWaterLocalLightScreenShadow(waterWorldPos, position), 1.0), 0.08, 1.0);
    vec3 radiance = FiniteOr(color, vec3(0.0)) * max(FiniteOr(intensity, 0.0), 0.0) *
        attenuation * localShadow;
    radiance = FiniteOr(radiance, vec3(0.0));
    rawRadianceEvidence = max(rawRadianceEvidence, Luminance(radiance));
    localShadowBlocked = max(localShadowBlocked, 1.0 - localShadow);
    localShadowPath = max(localShadowPath, 1.0);
    AccumulateHPWaterRadiance(radiance, localL, N, V, roughness, F, energyCompensation,
                              directSpecular, specularContribution,
                              macroLight, thinSSSLight, backlitLight,
                              macroContribution, thinSSSContribution, backlitContribution);
}

void AccumulateHPWaterAreaLight(int i,
                                vec3 waterWorldPos,
                                vec3 N,
                                vec3 V,
                                float roughness,
                                vec3 F,
                                float energyCompensation,
                                inout vec3 directSpecular,
                                inout vec3 areaSpecularContribution,
                                inout vec3 macroLight,
                                inout vec3 thinSSSLight,
                                inout vec3 backlitLight,
                                inout vec3 areaMacroContribution,
                                inout vec3 areaThinSSSContribution,
                                inout vec3 areaBacklitContribution,
                                inout float areaLocalShadowBlocked,
                                inout float areaLocalShadowPath,
                                inout float areaRawRadianceEvidence) {
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
        return;
    }

    vec3 areaCenter = u_AreaLightPositions[i];
    if (u_HPWaterLightPayloadEnabled == 1 && i < u_HPWaterAreaLightPayloadCount) {
        areaCenter = u_HPWaterAreaLightPayload[i * 5 + 0].xyz;
    }
    float localShadow = clamp(FiniteOr(ComputeHPWaterLocalLightScreenShadow(waterWorldPos, areaCenter), 1.0), 0.08, 1.0);
    radiance *= localShadow;
    radiance = FiniteOr(radiance, vec3(0.0));
    areaRawRadianceEvidence = max(areaRawRadianceEvidence, Luminance(radiance));
    areaLocalShadowBlocked = max(areaLocalShadowBlocked, 1.0 - localShadow);
    areaLocalShadowPath = max(areaLocalShadowPath, 1.0);

    vec3 areaSpecular = EvaluateHPWaterSpecularLight(
        N, V, localL, radiance, roughness, F, energyCompensation);
    directSpecular += areaSpecular;
    areaSpecularContribution += areaSpecular;
    float localNdotLRaw = dot(N, localL);
    float localNdotL = clamp(localNdotLRaw, 0.0, 1.0);
    float localTEntry = 1.0 - SchlickFresnel(localNdotL, HPWATER_WATER_F0);
    vec3 areaBodyRadiance = radiance * areaDiffuseScale;
    vec3 areaMacro = areaBodyRadiance * localNdotL * localTEntry;
    vec3 areaMediumScatter = areaBodyRadiance * 0.08;
    vec3 areaThinSSS = areaBodyRadiance * max(1.0 - localNdotL, 0.08);
    vec3 areaBacklit = areaBodyRadiance * clamp(-localNdotLRaw, 0.0, 1.0);
    macroLight += areaMacro;
    thinSSSLight += areaThinSSS + areaMediumScatter;
    backlitLight += areaBacklit;
    areaMacroContribution += areaMacro;
    areaThinSSSContribution += areaThinSSS + areaMediumScatter;
    areaBacklitContribution += areaBacklit;
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

    float expFactor = HPWaterAdaptiveRefractionExpFactor(maxCrossDistance);
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

    vec2 hitUV = clamp(hitNDC.xy, vec2(0.001), vec2(0.999));
    vec2 uvOffset = hitUV - uv;
    float edgeFade = HPWaterRefractionBoundFade(hitUV);
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
        RefractMeta = vec4(0.0, 0.0, 1.0, 0.0);
        SSRDiagnostics = vec4(0.0);
        AreaLightDiagnostics = vec4(0.0);
        ForwardScatterDiagnostics = vec4(0.0);
        PunctualLightDiagnostics = vec4(0.0);
        LocalLightShadowDiagnostics = vec4(0.0);
        return;
    }

    float mergedSceneDepth = texture(u_SceneDepth, v_UV).r;
    float sceneDepth = SampleSceneDepth(v_UV, 0.0);

    // Preserve foreground opaque objects. GL depth is smaller when closer.
    if (mergedSceneDepth < waterDepth - 0.00005) {
        FragColor = sceneColor;
        RefractData = vec4(0.0);
        RefractMeta = vec4(0.0, 0.0, mergedSceneDepth, 0.0);
        SSRDiagnostics = vec4(0.0);
        AreaLightDiagnostics = vec4(0.0);
        ForwardScatterDiagnostics = vec4(0.0);
        PunctualLightDiagnostics = vec4(0.0);
        LocalLightShadowDiagnostics = vec4(0.0);
        return;
    }

    vec4 normalRoughness = texture(u_HPWaterNormalRoughness, v_UV);
    vec4 scatterThickness = texture(u_HPWaterScatterThickness, v_UV);
    vec4 absorptionFoam = texture(u_HPWaterAbsorptionFoam, v_UV);

    vec3 N = DecodeHPWaterNormalRoughness(normalRoughness);
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
    if (refractedSceneDepth < 0.9999 && refractedWorldPos.y > waterWorldPos.y + 0.01) {
        refractUV = v_UV;
        refractedSceneDepth = sceneDepth;
        refractedColor = texture(u_SceneColor, refractUV).rgb;
        worldDepth = refractedSceneDepth >= 0.9999 ? waterDepth : refractedSceneDepth;
        refractedWorldPos = refractedSceneDepth >= 0.9999
            ? waterWorldPos
            : ReconstructWorldPosition(refractUV, worldDepth);
    }
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
    vec3 punctualSpecularContribution = vec3(0.0);
    vec3 punctualMacroContribution = vec3(0.0);
    vec3 punctualThinSSSContribution = vec3(0.0);
    vec3 punctualBacklitContribution = vec3(0.0);
    vec3 punctualMacroLight = vec3(0.0);
    vec3 punctualThinSSSLight = vec3(0.0);
    vec3 punctualBacklitLight = vec3(0.0);

    int pointCount = u_HPWaterLightPayloadEnabled == 1
        ? max(u_HPWaterPointLightPayloadCount, 0)
        : clamp(u_NumPointLights, 0, 8);
    int spotCount = u_HPWaterLightPayloadEnabled == 1
        ? max(u_HPWaterSpotLightPayloadCount, 0)
        : clamp(u_NumSpotLights, 0, 4);

    vec3 areaSpecularContribution = vec3(0.0);
    vec3 areaMacroContribution = vec3(0.0);
    vec3 areaThinSSSContribution = vec3(0.0);
    vec3 areaBacklitContribution = vec3(0.0);
    float punctualLocalShadowBlocked = 0.0;
    float punctualLocalShadowPath = 0.0;
    float areaLocalShadowBlocked = 0.0;
    float areaLocalShadowPath = 0.0;
    float punctualRawRadianceEvidence = 0.0;
    float areaRawRadianceEvidence = 0.0;

    int areaCount = u_HPWaterLightPayloadEnabled == 1
        ? max(u_HPWaterAreaLightPayloadCount, 0)
        : clamp(u_NumAreaLights, 0, 4);
    int tiledReferenceOffset = 0;
    int tiledReferenceCount = 0;
    bool usedTiledLightList = HPWaterFetchTiledLightList(gl_FragCoord.xy,
                                                         tiledReferenceOffset,
                                                         tiledReferenceCount);
    if (usedTiledLightList) {
        int clampedReferenceCount =
            min(tiledReferenceCount,
                u_HPWaterTiledLightListReferenceCount - tiledReferenceOffset);
        for (int refIt = 0; refIt < HPWATER_MAX_TILE_LIGHT_REFERENCES; ++refIt) {
            if (refIt >= clampedReferenceCount) {
                break;
            }

            uint referenceValue = HPWaterReadTiledLightReference(tiledReferenceOffset + refIt);
            int lightIndex = HPWaterDecodeLightReferenceIndex(referenceValue);
            if ((referenceValue & HPWATER_LIGHT_REF_AREA_FLAG) != 0u) {
                if (lightIndex < areaCount) {
                    AccumulateHPWaterAreaLight(lightIndex, waterWorldPos, N, V, roughness, F,
                                               energyCompensation,
                                               directSpecular, areaSpecularContribution,
                                               punctualMacroLight, punctualThinSSSLight,
                                               punctualBacklitLight,
                                               areaMacroContribution, areaThinSSSContribution,
                                               areaBacklitContribution,
                                               areaLocalShadowBlocked,
                                               areaLocalShadowPath,
                                               areaRawRadianceEvidence);
                }
            } else if ((referenceValue & HPWATER_LIGHT_REF_SPOT_FLAG) != 0u) {
                if (lightIndex < spotCount) {
                    AccumulateHPWaterSpotLight(lightIndex, waterWorldPos, N, V, roughness, F,
                                               energyCompensation,
                                               directSpecular, punctualSpecularContribution,
                                               punctualMacroLight, punctualThinSSSLight,
                                               punctualBacklitLight,
                                               punctualMacroContribution, punctualThinSSSContribution,
                                               punctualBacklitContribution,
                                               punctualLocalShadowBlocked,
                                               punctualLocalShadowPath,
                                               punctualRawRadianceEvidence);
                }
            } else if (lightIndex < pointCount) {
                AccumulateHPWaterPointLight(lightIndex, waterWorldPos, N, V, roughness, F,
                                            energyCompensation,
                                            directSpecular, punctualSpecularContribution,
                                            punctualMacroLight, punctualThinSSSLight,
                                            punctualBacklitLight,
                                            punctualMacroContribution, punctualThinSSSContribution,
                                            punctualBacklitContribution,
                                            punctualLocalShadowBlocked,
                                            punctualLocalShadowPath,
                                            punctualRawRadianceEvidence);
            }
        }
    }

    bool tiledLightListProducedContribution =
        Luminance(punctualSpecularContribution + punctualMacroContribution +
                  punctualThinSSSContribution + punctualBacklitContribution +
                  areaSpecularContribution + areaMacroContribution +
                  areaThinSSSContribution + areaBacklitContribution) > 0.000001 ||
        punctualRawRadianceEvidence > 0.000001 ||
        areaRawRadianceEvidence > 0.000001 ||
        punctualLocalShadowPath > 0.000001 ||
        areaLocalShadowPath > 0.000001;
    if (!usedTiledLightList || !tiledLightListProducedContribution) {
        for (int i = 0; i < 8; ++i) {
            if (i >= pointCount) {
                break;
            }
            AccumulateHPWaterPointLight(i, waterWorldPos, N, V, roughness, F,
                                        energyCompensation,
                                        directSpecular, punctualSpecularContribution,
                                        punctualMacroLight, punctualThinSSSLight,
                                        punctualBacklitLight,
                                        punctualMacroContribution, punctualThinSSSContribution,
                                        punctualBacklitContribution,
                                        punctualLocalShadowBlocked,
                                        punctualLocalShadowPath,
                                        punctualRawRadianceEvidence);
        }
        for (int i = 0; i < 4; ++i) {
            if (i >= spotCount) {
                break;
            }
            AccumulateHPWaterSpotLight(i, waterWorldPos, N, V, roughness, F,
                                       energyCompensation,
                                       directSpecular, punctualSpecularContribution,
                                       punctualMacroLight, punctualThinSSSLight,
                                       punctualBacklitLight,
                                       punctualMacroContribution, punctualThinSSSContribution,
                                       punctualBacklitContribution,
                                       punctualLocalShadowBlocked,
                                       punctualLocalShadowPath,
                                       punctualRawRadianceEvidence);
        }
        for (int i = 0; i < 4; ++i) {
            if (i >= areaCount) {
                break;
            }
            AccumulateHPWaterAreaLight(i, waterWorldPos, N, V, roughness, F,
                                       energyCompensation,
                                       directSpecular, areaSpecularContribution,
                                       punctualMacroLight, punctualThinSSSLight,
                                       punctualBacklitLight,
                                       areaMacroContribution, areaThinSSSContribution,
                                       areaBacklitContribution,
                                       areaLocalShadowBlocked,
                                       areaLocalShadowPath,
                                       areaRawRadianceEvidence);
        }
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
        HPWaterCalculateMipLevel(
            hpWaterRayLength,
            HPWATER_FORWARD_SCALING_FACTOR,
            scatterDensity,
            clamp(u_ForwardScatterBlurDensity, 0.0, 4.0),
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

    vec3 foamTint = max(u_HPFoamColor, vec3(0.0));
    vec3 foamColor = mix(foamTint * 0.88, foamTint, foam);
    vec3 waterColor = mix(exitedBodyColor + reflected, foamColor, foam * 0.65);
    float waterAlpha = clamp(0.28 + normalizedThickness * 0.62 + foam * 0.45, 0.0, 0.92);

    FragColor = vec4(mix(sceneColor.rgb, waterColor, waterAlpha), sceneColor.a);
    RefractData = vec4(refractedWorldPos, rayLength);
    RefractMeta = vec4(refractUV - v_UV, refractedSceneDepth, 1.0);
    SSRDiagnostics = vec4(ssrConfidence,
                          ssrHit,
                          probeHierarchyWeight,
                          skyHierarchyWeight);
    AreaLightDiagnostics = vec4(
        max(Luminance(areaSpecularContribution), areaRawRadianceEvidence),
        max(Luminance(areaMacroContribution), areaRawRadianceEvidence),
        max(Luminance(areaThinSSSContribution + areaBacklitContribution), areaRawRadianceEvidence),
        areaCount > 0 ? 1.0 : 0.0);
    float forwardMipUsed =
        (u_SceneColorMipEnabled == 1 &&
         u_SceneColorMipCount > 1 &&
         forwardStrength > 0.0001 &&
         forwardBlurLOD > 0.999)
            ? 1.0
            : 0.0;
    float forwardMipLOD =
        forwardMipUsed * clamp(forwardBlurLOD / max(float(u_SceneColorMipCount - 1), 1.0), 0.0, 1.0);
    ForwardScatterDiagnostics = vec4(
        forwardMipLOD,
        Luminance(forwardBlur) * forwardStrength,
        scatterDensity,
        forwardMipUsed);
    PunctualLightDiagnostics = vec4(
        max(Luminance(punctualSpecularContribution), punctualRawRadianceEvidence),
        max(Luminance(punctualMacroContribution), punctualRawRadianceEvidence),
        max(Luminance(punctualThinSSSContribution + punctualBacklitContribution), punctualRawRadianceEvidence),
        (pointCount + spotCount) > 0 ? 1.0 : 0.0);
    LocalLightShadowDiagnostics = vec4(
        punctualLocalShadowBlocked,
        areaLocalShadowBlocked,
        max(punctualLocalShadowBlocked, areaLocalShadowBlocked),
        max(punctualLocalShadowPath, areaLocalShadowPath));
}
#endif

            ENDGLSL
        }
    }

    FallBack "VibeEngine/DeferredLighting"
}
