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

uniform float u_NearClip;
uniform float u_FarClip;
uniform int u_HPWaterMaskEnabled;
uniform int u_HPWaterCausticEnabled;
uniform float u_CausticVolumeStrength;
uniform float u_MacroScatterStrength;
uniform int u_VolumeSampleCount;
uniform int u_FrameIndex;
uniform vec3 u_LightDir;
uniform vec3 u_LightColor;
uniform float u_LightIntensity;
uniform vec3 u_CameraPosition;
uniform mat4 u_InverseViewProjection;

#include "shadows.glslinc"

const float PI = 3.14159265358979323846;

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

float InterleavedGradientNoise(vec2 pixelPos, int frameIndex) {
    const vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
    vec2 scrolled = pixelPos + vec2(float(frameIndex & 63)) * vec2(5.588238, 5.588238);
    return fract(magic.z * fract(dot(scrolled, magic.xy)));
}

vec3 SafeNormalize(vec3 v, vec3 fallback) {
    float len2 = dot(v, v);
    return len2 > 0.000001 ? v * inversesqrt(len2) : fallback;
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
        vec3 phase = ScatterPhase(cosTheta);
        float shadowVisibility = ComputeShadow(samplePos, N, 0.0);
        float volumeShadow = mix(0.35, 1.0, shadowVisibility);
        vec3 stepTransmittance = exp(-extinction * segment);
        vec3 extinguished = vec3(1.0) - stepTransmittance;
        float depthWeight = 0.55 + 0.45 * smoothstep(0.0, 1.0, midD);

        directScatter += accumTransmittance * baseDirectLight * volumeShadow * extinguished *
            scatteringAlbedo * (phase * 2.6) * macroScatter * depthWeight;

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
