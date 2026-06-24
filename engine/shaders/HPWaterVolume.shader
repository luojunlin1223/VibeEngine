// VibeEngine ShaderLab - HPWater low-resolution volume pass
// Accumulates water in-scattering/transmittance from the dedicated HPWater
// G-buffer and precomputed refraction payload. Temporal reprojection and
// a-trous filtering are handled by the follow-up HPWater volume passes.

Shader "VibeEngine/HPWaterVolume" {
    Properties {
    }

    SubShader {
        Tags { "RenderType"="Opaque" "Queue"="Overlay" "LightMode"="HPWaterVolume" }

        Pass {
            Name "HPWaterVolumePass"

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
layout(location = 0) out vec4 VolumeColor;
layout(location = 1) out vec4 VolumeTransmittance;
layout(location = 2) out vec4 VolumeDepth;

uniform sampler2D u_SceneColor;
uniform sampler2D u_HPWaterNormalRoughness;
uniform sampler2D u_HPWaterScatterThickness;
uniform sampler2D u_HPWaterAbsorptionFoam;
uniform sampler2D u_HPWaterDepth;
uniform sampler2D u_HPWaterRefractionWorldData;
uniform sampler2D u_HPWaterRefractionMeta;
uniform sampler2D u_HPWaterMask;
uniform sampler2D u_HPWaterCaustic;
uniform sampler2D u_AreaLightLTCLUT;

uniform float u_NearClip;
uniform float u_FarClip;
uniform int u_HPWaterMaskEnabled;
uniform int u_HPWaterCausticEnabled;
uniform float u_CausticVolumeStrength;
uniform float u_MacroScatterStrength;
uniform int u_HPWaterVolumeShadowParamsEnabled;
uniform float u_HPWaterVolumeShadowSoftness;
uniform float u_HPWaterVolumeShadowMinFilterSize;
uniform int u_HPWaterVolumeShadowBlockerSamples;
uniform int u_HPWaterVolumeShadowFilterSamples;
uniform int u_VolumeSampleCount;
uniform int u_FrameIndex;
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
uniform int u_AreaLightLTCLUTEnabled;
uniform vec3 u_AreaLightPositions[4];
uniform vec3 u_AreaLightRights[4];
uniform vec3 u_AreaLightUps[4];
uniform vec3 u_AreaLightForwards[4];
uniform vec3 u_AreaLightColors[4];
uniform float u_AreaLightIntensities[4];
uniform float u_AreaLightRanges[4];
uniform float u_AreaLightWidths[4];
uniform float u_AreaLightHeights[4];
uniform vec3 u_CameraPosition;
uniform mat4 u_InverseViewProjection;

#include "shadows.glslinc"

const float PI = 3.14159265358979323846;
const vec2 HPWATER_VOLUME_SHADOW_OFFSETS[16] = vec2[16](
    vec2(0.0, 0.0),
    vec2(0.5381, 0.1856),
    vec2(-0.4319, 0.2485),
    vec2(0.0797, -0.4905),
    vec2(-0.3565, -0.6235),
    vec2(0.3328, -0.7964),
    vec2(0.7750, -0.4321),
    vec2(-0.7463, -0.2420),
    vec2(-0.9000, 0.4137),
    vec2(0.8349, 0.4614),
    vec2(-0.2302, 0.9187),
    vec2(0.4075, 0.8226),
    vec2(-0.7221, -0.6911),
    vec2(0.9715, -0.0713),
    vec2(-0.0762, -0.9458),
    vec2(0.1918, 0.4931)
);

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

float Luminance(vec3 color) {
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

float HenyeyGreenstein(float cosTheta, float g) {
    float g2 = g * g;
    float denom = max(1.0 + g2 - 2.0 * g * cosTheta, 0.001);
    return (1.0 - g2) / (4.0 * PI * pow(denom, 1.5));
}

vec3 ScatterPhase(float cosTheta) {
    vec3 betaRayleigh = vec3(5.8e-6, 13.5e-6, 33.1e-6) * 1.0e6;
    float rayleighPhase = (1.0 + cosTheta * cosTheta) * (3.0 / (16.0 * PI));
    float miePhase = HenyeyGreenstein(cosTheta, 0.72);
    return betaRayleigh * rayleighPhase * 0.05 + vec3(miePhase) * 0.95;
}

vec3 HPWaterEffectiveScatterPhase(float cosTheta, vec3 scatteringAlbedo) {
    vec3 phase = ScatterPhase(cosTheta);
    float albedoScalar = clamp(Luminance(scatteringAlbedo), 0.0, 1.0);
    float isotropicWeight = smoothstep(0.0, 0.5, albedoScalar);
    return mix(phase, vec3(1.0), isotropicWeight);
}

float InterleavedGradientNoise(vec2 pixelPos, int frameIndex) {
    const vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
    vec2 scrolled = pixelPos + vec2(float(frameIndex & 63)) * vec2(5.588238, 5.588238);
    return fract(magic.z * fract(dot(scrolled, magic.xy)));
}

vec3 SafeNormalize(vec3 v, vec3 fallback) {
    float len2 = dot(v, v);
    return len2 > 0.000001 ? v * inversesqrt(len2) : fallback;
}

float HPWaterRangeAttenuation(float distanceToLight, float range) {
    float safeRange = max(range, 0.001);
    float range01 = clamp(distanceToLight / safeRange, 0.0, 1.0);
    float rangeWindow = 1.0 - pow(range01, 4.0);
    rangeWindow *= rangeWindow;
    return rangeWindow / (distanceToLight * distanceToLight + 1.0);
}

vec4 SampleHPWaterAreaLightLTC(float roughness, float nDotV) {
    if (u_AreaLightLTCLUTEnabled != 1) {
        return vec4(1.0, 0.5, 0.35, 1.0);
    }

    return texture(u_AreaLightLTCLUT,
        vec2(clamp(nDotV, 0.0, 1.0), clamp(roughness, 0.0, 1.0)));
}

void AccumulateHPWaterAreaLightSample(vec3 samplePos,
                                      vec3 samplePoint,
                                      vec3 forward,
                                      float lightRange,
                                      inout vec3 weightedDirection,
                                      inout float weightSum) {
    vec3 lightVector = samplePoint - samplePos;
    float lightDistance = length(lightVector);
    if (lightDistance <= 0.0001 || lightDistance >= lightRange) {
        return;
    }

    vec3 sampleL = lightVector / lightDistance;
    float emissionFacing = clamp(dot(forward, -sampleL), 0.0, 1.0);
    float sampleWeight = HPWaterRangeAttenuation(lightDistance, lightRange) * emissionFacing;
    weightedDirection += sampleL * sampleWeight;
    weightSum += sampleWeight;
}

vec3 ComputeHPWaterAreaLightRadiance(vec3 samplePos,
                                     vec3 N,
                                     vec3 sampleToCamera,
                                     float roughness,
                                     int lightIndex,
                                     out vec3 Lp) {
    vec3 center = u_AreaLightPositions[lightIndex];
    vec3 right = SafeNormalize(u_AreaLightRights[lightIndex], vec3(1.0, 0.0, 0.0));
    vec3 up = SafeNormalize(u_AreaLightUps[lightIndex], vec3(0.0, 1.0, 0.0));
    vec3 forward = SafeNormalize(u_AreaLightForwards[lightIndex], vec3(0.0, 0.0, 1.0));
    float width = max(u_AreaLightWidths[lightIndex], 0.001);
    float height = max(u_AreaLightHeights[lightIndex], 0.001);
    float lightRange = max(u_AreaLightRanges[lightIndex], 0.001);
    vec2 halfSize = vec2(width, height) * 0.5;
    vec4 ltc = SampleHPWaterAreaLightLTC(roughness, clamp(dot(N, sampleToCamera), 0.0, 1.0));
    vec2 ltcHalfSize = halfSize * mix(0.45, 1.0, clamp(ltc.g, 0.0, 1.0));

    vec3 weightedDirection = vec3(0.0);
    float weightSum = 0.0;
    AccumulateHPWaterAreaLightSample(samplePos, center, forward, lightRange, weightedDirection, weightSum);
    AccumulateHPWaterAreaLightSample(samplePos, center + right * ltcHalfSize.x + up * ltcHalfSize.y,
        forward, lightRange, weightedDirection, weightSum);
    AccumulateHPWaterAreaLightSample(samplePos, center - right * ltcHalfSize.x + up * ltcHalfSize.y,
        forward, lightRange, weightedDirection, weightSum);
    AccumulateHPWaterAreaLightSample(samplePos, center + right * ltcHalfSize.x - up * ltcHalfSize.y,
        forward, lightRange, weightedDirection, weightSum);
    AccumulateHPWaterAreaLightSample(samplePos, center - right * ltcHalfSize.x - up * ltcHalfSize.y,
        forward, lightRange, weightedDirection, weightSum);

    Lp = SafeNormalize(weightedDirection, forward);
    if (weightSum <= 0.0) {
        return vec3(0.0);
    }

    float areaScale = clamp(width * height * 0.25, 0.1, 8.0);
    return u_AreaLightColors[lightIndex] *
        max(u_AreaLightIntensities[lightIndex], 0.0) *
        (weightSum / 5.0) * areaScale * max(ltc.r * ltc.a, 0.0);
}

vec3 ComputeHPWaterVolumePunctualLighting(vec3 samplePos,
                                          vec3 sampleToCamera,
                                          vec3 N,
                                          float roughness,
                                          vec3 scatteringAlbedo,
                                          float normalizedThickness) {
    vec3 punctual = vec3(0.0);
    int pointCount = clamp(u_NumPointLights, 0, 8);
    for (int i = 0; i < 8; ++i) {
        if (i >= pointCount)
            break;

        vec3 lightVector = u_PointLightPositions[i] - samplePos;
        float lightDistance = length(lightVector);
        float lightRange = max(u_PointLightRanges[i], 0.001);
        if (lightDistance <= 0.0001 || lightDistance >= lightRange)
            continue;

        vec3 Lp = lightVector / lightDistance;
        float attenuation = HPWaterRangeAttenuation(lightDistance, lightRange);
        float cosTheta = clamp(dot(sampleToCamera, Lp), -1.0, 1.0);
        vec3 phase = HPWaterEffectiveScatterPhase(cosTheta, scatteringAlbedo);
        vec3 radiance = u_PointLightColors[i] *
            max(u_PointLightIntensities[i], 0.0) * attenuation;
        punctual += radiance * phase * 2.2;
    }

    int spotCount = clamp(u_NumSpotLights, 0, 4);
    for (int i = 0; i < 4; ++i) {
        if (i >= spotCount)
            break;

        vec3 lightVector = u_SpotLightPositions[i] - samplePos;
        float lightDistance = length(lightVector);
        float lightRange = max(u_SpotLightRanges[i], 0.001);
        if (lightDistance <= 0.0001 || lightDistance >= lightRange)
            continue;

        vec3 Lp = lightVector / lightDistance;
        vec3 spotForward = SafeNormalize(-u_SpotLightDirections[i], vec3(0.0, -1.0, 0.0));
        float theta = dot(Lp, spotForward);
        float epsilon = max(u_SpotLightInnerCos[i] - u_SpotLightOuterCos[i], 0.001);
        float spotFactor = clamp((theta - u_SpotLightOuterCos[i]) / epsilon, 0.0, 1.0);
        spotFactor *= spotFactor;
        if (spotFactor <= 0.0001)
            continue;

        float attenuation = HPWaterRangeAttenuation(lightDistance, lightRange) * spotFactor;
        float cosTheta = clamp(dot(sampleToCamera, Lp), -1.0, 1.0);
        vec3 phase = HPWaterEffectiveScatterPhase(cosTheta, scatteringAlbedo);
        vec3 radiance = u_SpotLightColors[i] *
            max(u_SpotLightIntensities[i], 0.0) * attenuation;
        punctual += radiance * phase * 2.2;
    }

    int areaCount = clamp(u_NumAreaLights, 0, 4);
    for (int i = 0; i < 4; ++i) {
        if (i >= areaCount)
            break;

        vec3 Lp = vec3(0.0, 1.0, 0.0);
        vec3 radiance = ComputeHPWaterAreaLightRadiance(samplePos,
                                                        N,
                                                        sampleToCamera,
                                                        roughness,
                                                        i,
                                                        Lp);
        if (max(max(radiance.r, radiance.g), radiance.b) <= 0.00001)
            continue;

        float cosTheta = clamp(dot(sampleToCamera, Lp), -1.0, 1.0);
        punctual += radiance * HPWaterEffectiveScatterPhase(cosTheta, scatteringAlbedo) * 2.2;
    }

    return punctual * (0.55 + 0.45 * normalizedThickness);
}

float SampleHPWaterVolumeShadowCascade(vec3 shadowCoord,
                                       int cascade,
                                       float bias,
                                       float radiusTexels,
                                       int sampleCount) {
    if (shadowCoord.x < 0.0 || shadowCoord.x > 1.0 ||
        shadowCoord.y < 0.0 || shadowCoord.y > 1.0 ||
        shadowCoord.z < 0.0 || shadowCoord.z > 1.0)
        return 1.0;

    vec2 texelSize = 1.0 / vec2(textureSize(u_ShadowMap, 0).xy);
    int clampedSampleCount = clamp(sampleCount, 1, 16);
    float clampedRadius = max(radiusTexels, 0.0);
    float visibility = 0.0;
    for (int i = 0; i < 16; ++i) {
        if (i >= clampedSampleCount)
            break;
        vec2 offset = HPWATER_VOLUME_SHADOW_OFFSETS[i] * texelSize * clampedRadius;
        visibility += SampleShadowHard(vec3(shadowCoord.xy + offset, shadowCoord.z),
                                       cascade,
                                       bias);
    }
    return visibility / float(clampedSampleCount);
}

float EstimateHPWaterVolumeBlockerRatio(vec3 shadowCoord,
                                        int cascade,
                                        float bias,
                                        float radiusTexels,
                                        int sampleCount) {
    int clampedSampleCount = clamp(sampleCount, 0, 16);
    if (clampedSampleCount <= 0)
        return 1.0 - SampleShadowHard(shadowCoord, cascade, bias);

    float visibility = SampleHPWaterVolumeShadowCascade(shadowCoord,
                                                        cascade,
                                                        bias,
                                                        max(radiusTexels, 1.0),
                                                        clampedSampleCount);
    return clamp(1.0 - visibility, 0.0, 1.0);
}

float ComputeHPWaterVolumeShadow(vec3 fragPosWorld, vec3 N) {
    if (u_HPWaterVolumeShadowParamsEnabled == 0)
        return ComputeShadow(fragPosWorld, N, 0.0);

    if (u_ShadowsEnabled == 0)
        return 1.0;

    float viewDepth = abs((u_ShadowCameraView * vec4(fragPosWorld, 1.0)).z);
    if (viewDepth > u_ShadowCascadeSplits[CSM_NUM_CASCADES - 1])
        return 1.0;

    int cascade = SelectCascade(viewDepth);
    vec3 biasedPos = ShadowBiasedPosition(fragPosWorld, N);
    float receiverBias = ShadowReceiverDepthBias(N);
    float minFilterSize = clamp(u_HPWaterVolumeShadowMinFilterSize, 0.0, 8.0);
    float softness = clamp(u_HPWaterVolumeShadowSoftness, 0.0, 10.0);
    int blockerSamples = clamp(u_HPWaterVolumeShadowBlockerSamples, 0, 16);
    int filterSamples = clamp(u_HPWaterVolumeShadowFilterSamples, 1, 16);

    vec3 shadowCoord = ProjectToShadowCoord(biasedPos, cascade);
    float blockerRatio = EstimateHPWaterVolumeBlockerRatio(shadowCoord,
                                                           cascade,
                                                           receiverBias,
                                                           max(minFilterSize, 1.0),
                                                           blockerSamples);
    float filterRadius = max(minFilterSize, 0.0) + softness * (0.35 + blockerRatio * 1.65);
    float shadow = SampleHPWaterVolumeShadowCascade(shadowCoord,
                                                    cascade,
                                                    receiverBias,
                                                    filterRadius,
                                                    filterSamples);

    if (cascade < CSM_NUM_CASCADES - 1 && u_ShadowCascadeBlendWidth > 0.0) {
        float cascadeEnd = u_ShadowCascadeSplits[cascade];
        float blendStart = cascadeEnd * (1.0 - u_ShadowCascadeBlendWidth);
        if (viewDepth > blendStart) {
            float blendFactor = clamp(
                (viewDepth - blendStart) / max(cascadeEnd - blendStart, 0.0001),
                0.0,
                1.0);
            int nextCascade = cascade + 1;
            vec3 nextCoord = ProjectToShadowCoord(biasedPos, nextCascade);
            float nextBlockerRatio = EstimateHPWaterVolumeBlockerRatio(nextCoord,
                                                                       nextCascade,
                                                                       receiverBias,
                                                                       max(minFilterSize, 1.0),
                                                                       blockerSamples);
            float nextFilterRadius = max(minFilterSize, 0.0) +
                softness * (0.35 + nextBlockerRatio * 1.65);
            float nextShadow = SampleHPWaterVolumeShadowCascade(nextCoord,
                                                                nextCascade,
                                                                receiverBias,
                                                                nextFilterRadius,
                                                                filterSamples);
            if (u_ShadowCascadeDitherEnabled != 0) {
                float dither = ShadowInterleavedGradientNoise(gl_FragCoord.xy, u_ShadowFrameIndex);
                if (dither < blendFactor) {
                    shadow = nextShadow;
                }
            } else {
                shadow = mix(shadow, nextShadow, blendFactor);
            }
        }
    }

    return clamp(shadow, 0.0, 1.0);
}

void main() {
    float waterDepth = texture(u_HPWaterDepth, v_UV).r;
    vec4 refractData = texture(u_HPWaterRefractionWorldData, v_UV);
    vec4 refractMeta = texture(u_HPWaterRefractionMeta, v_UV);
    float waterMask = u_HPWaterMaskEnabled == 1
        ? texture(u_HPWaterMask, v_UV).r
        : (waterDepth < 0.9999 ? 1.0 : 0.0);

    if (waterMask < 0.5 || waterDepth >= 0.9999 || refractMeta.w <= 0.0001) {
        VolumeColor = vec4(0.0);
        VolumeTransmittance = vec4(1.0, 1.0, 1.0, 0.0);
        VolumeDepth = vec4(0.0);
        return;
    }

    vec4 normalRoughness = texture(u_HPWaterNormalRoughness, v_UV);
    vec4 scatterThickness = texture(u_HPWaterScatterThickness, v_UV);
    vec4 absorptionFoam = texture(u_HPWaterAbsorptionFoam, v_UV);

    vec3 N = normalize(normalRoughness.xyz * 2.0 - 1.0);
    vec3 scatterColor = max(scatterThickness.rgb, vec3(0.0));
    float depthTintDistance = max(scatterThickness.a, 0.1);
    vec3 absorptionColor = max(absorptionFoam.rgb, vec3(0.0001));
    float foam = clamp(absorptionFoam.a, 0.0, 1.0);

    vec3 waterWorldPos = ReconstructWorldPosition(v_UV, waterDepth);
    vec3 refractedWorldPos = refractData.xyz;
    float rayLength = refractData.w;
    float normalizedThickness = clamp(refractMeta.w, 0.0, 1.0);
    if (rayLength <= 0.001) {
        rayLength = max(normalizedThickness * depthTintDistance, 0.01);
    }

    vec3 V = SafeNormalize(u_CameraPosition - waterWorldPos, vec3(0.0, 1.0, 0.0));
    vec3 L = normalize(u_LightDir);
    float NdotL = clamp(dot(N, L), 0.0, 1.0);

    vec3 absorptionCoeff = absorptionColor * 1.35;
    vec3 scatterCoeff = max(scatterColor, vec3(0.0001)) * 0.42;
    vec3 extinction = max(absorptionCoeff + scatterCoeff, vec3(0.0001));
    vec3 scatteringAlbedo = scatterCoeff / extinction;
    float macroScatter = clamp(u_MacroScatterStrength, 0.0, 4.0);

    vec3 baseDirectLight = u_LightColor * max(u_LightIntensity, 0.0) *
        (0.18 + 0.82 * NdotL) * (0.75 + 0.25 * normalizedThickness);
    if (u_HPWaterCausticEnabled == 1) {
        vec4 caustic = texture(u_HPWaterCaustic, v_UV);
        float causticWeight = clamp(caustic.a, 0.0, 1.0) *
            (0.25 + 0.75 * normalizedThickness);
        baseDirectLight += caustic.rgb * causticWeight * clamp(u_CausticVolumeStrength, 0.0, 4.0);
    }

    const float expFactor = 8.0;
    int sampleCount = clamp(u_VolumeSampleCount, 4, 32);
    vec2 volumeSize = vec2(max(textureSize(u_HPWaterDepth, 0), ivec2(1)));
    float dither = InterleavedGradientNoise(v_UV * volumeSize, u_FrameIndex);
    float invSampleCount = 1.0 / float(sampleCount);
    float previousD = 0.0;
    vec3 accumTransmittance = vec3(1.0);
    vec3 directScatter = vec3(0.0);
    vec3 sceneInScatter = vec3(0.0);
    float accumulatedDistance = 0.0;

    for (int i = 0; i < 32; ++i) {
        if (i >= sampleCount)
            break;

        float stepT = (float(i) + dither) * invSampleCount;
        float d = (pow(expFactor, stepT) - 1.0) / (expFactor - 1.0);
        if (i == sampleCount - 1) {
            d = 1.0;
        }

        float segment = max((d - previousD) * rayLength, 0.0);
        float midD = clamp((previousD + d) * 0.5, 0.0, 1.0);
        vec3 samplePos = mix(waterWorldPos, refractedWorldPos, midD);
        vec2 sampleUV = mix(v_UV, clamp(refractMeta.xy, vec2(0.001), vec2(0.999)), midD);
        vec3 sampleToCamera = SafeNormalize(u_CameraPosition - samplePos, V);
        float cosTheta = clamp(dot(sampleToCamera, L), -1.0, 1.0);
        vec3 phase = HPWaterEffectiveScatterPhase(cosTheta, scatteringAlbedo);
        float shadowVisibility = ComputeHPWaterVolumeShadow(samplePos, N);
        float volumeShadow = mix(0.35, 1.0, shadowVisibility);
        vec3 stepTransmittance = exp(-extinction * segment);
        vec3 extinguished = vec3(1.0) - stepTransmittance;
        float depthWeight = 0.55 + 0.45 * smoothstep(0.0, 1.0, midD);

        vec3 directionalScatter = baseDirectLight * volumeShadow * (phase * 2.6);
        vec3 punctualScatter = ComputeHPWaterVolumePunctualLighting(samplePos,
                                                                    sampleToCamera,
                                                                    N,
                                                                    clamp(normalRoughness.w, 0.02, 1.0),
                                                                    scatteringAlbedo,
                                                                    normalizedThickness);
        directScatter += accumTransmittance * (directionalScatter + punctualScatter) *
            extinguished * scatteringAlbedo * macroScatter * depthWeight;

        vec3 refractedSceneColor = texture(u_SceneColor, sampleUV).rgb;
        sceneInScatter += accumTransmittance * refractedSceneColor * scatterCoeff *
            segment * (0.08 + 0.22 * normalizedThickness) * macroScatter;

        accumTransmittance *= stepTransmittance;
        accumulatedDistance += segment;
        previousD = d;
    }

    vec3 transmittance = clamp(accumTransmittance, vec3(0.0), vec3(1.0));
    if (accumulatedDistance <= 0.0001) {
        transmittance = exp(-extinction * rayLength);
    }

    vec3 ambientScatter = scatterColor * (0.025 + 0.12 * normalizedThickness) * macroScatter;
    vec3 volumeColor = directScatter + sceneInScatter + ambientScatter;
    volumeColor = mix(volumeColor, vec3(0.96, 0.98, 1.0), foam * 0.35);

    float refractedLinearDepth = refractMeta.z >= 0.9999
        ? LinearizeDepth(waterDepth) + rayLength
        : LinearizeDepth(refractMeta.z);

    VolumeColor = vec4(max(volumeColor, vec3(0.0)), 1.0);
    VolumeTransmittance = vec4(clamp(transmittance, vec3(0.0), vec3(1.0)), 1.0);
    VolumeDepth = vec4(refractedLinearDepth, rayLength, normalizedThickness, 1.0);
}
#endif

            ENDGLSL
        }
    }

    FallBack "VibeEngine/HPWaterComposite"
}
