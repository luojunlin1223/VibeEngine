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
layout(location = 3) out vec4 VolumeAreaLightDiagnostics;

uniform sampler2D u_SceneColor;
uniform sampler2D u_HPWaterNormalRoughness;
uniform sampler2D u_HPWaterScatterThickness;
uniform sampler2D u_HPWaterAbsorptionFoam;
uniform sampler2D u_HPWaterDepth;
uniform sampler2D u_HPWaterRefractionWorldData;
uniform sampler2D u_HPWaterRefractionMeta;
uniform sampler2D u_HPWaterMask;
uniform sampler2D u_HPWaterCaustic;
uniform sampler2DArray u_AreaLightLTCLUT;

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
uniform float u_MaxRefractionCrossDistance;
uniform vec3 u_LightDir;
uniform vec3 u_LightColor;
uniform float u_LightIntensity;
uniform float u_PhaseG;
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
uniform vec3 u_CameraPosition;
uniform mat4 u_InverseViewProjection;
uniform vec4 u_HPWaterVolumeResolution; // xy = low-res size, zw = 1 / low-res size

#include "shadows.glslinc"
#include "hpwater_normal.glslinc"

const float PI = 3.14159265358979323846;
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

vec2 ResolveHPWaterFullResUV() {
    vec2 fullSize = vec2(max(textureSize(u_HPWaterDepth, 0), ivec2(1)));
    vec2 lowSize = max(u_HPWaterVolumeResolution.xy, vec2(1.0));
    vec2 lowCoord = floor(gl_FragCoord.xy);
    vec2 fullPixel = floor((lowCoord + vec2(0.5)) * (fullSize / lowSize));
    return clamp((fullPixel + vec2(0.5)) / fullSize, vec2(0.0005), vec2(0.9995));
}

float Luminance(vec3 color) {
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

float HenyeyGreenstein(float cosTheta, float g) {
    float g2 = g * g;
    float denom = max(1.0 + g2 - 2.0 * g * cosTheta, 0.001);
    return (1.0 - g2) / (4.0 * PI * pow(denom, 1.5));
}

vec3 ScatterPhase(float cosTheta, float phaseG) {
    vec3 betaRayleigh = vec3(5.8e-6, 13.5e-6, 33.1e-6) * 1.0e6;
    float rayleighPhase = (1.0 + cosTheta * cosTheta) * (3.0 / (16.0 * PI));
    float miePhase = HenyeyGreenstein(cosTheta, clamp(phaseG, -0.95, 0.95));
    return betaRayleigh * rayleighPhase * 0.05 + vec3(miePhase) * 0.95;
}

vec3 HPWaterEffectiveScatterPhase(float cosTheta, vec3 scatteringAlbedo) {
    vec3 phase = ScatterPhase(cosTheta, u_PhaseG);
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

    tangent = SafeNormalize(tangent, vec3(1.0, 0.0, 0.0));
    vec3 bitangent = SafeNormalize(cross(N, tangent), vec3(0.0, 1.0, 0.0));
    return mat3(tangent, bitangent, N);
}

vec3 HPWaterLTCComputeEdgeFactor(vec3 v1, vec3 v2) {
    float cosTheta = clamp(dot(v1, v2), -0.9999, 0.9999);
    float theta = acos(cosTheta);
    float invSinTheta = inversesqrt(max(1.0 - cosTheta * cosTheta, 0.000001));
    return cross(v1, v2) * theta * invSinTheta;
}

vec3 HPWaterLTCPolygonFormFactor(vec3 l0, vec3 l1, vec3 l2, vec3 l3, vec3 l4, int vertexCount) {
    l0 = SafeNormalize(l0, vec3(0.0, 0.0, 1.0));
    l1 = SafeNormalize(l1, vec3(0.0, 0.0, 1.0));
    l2 = SafeNormalize(l2, vec3(0.0, 0.0, 1.0));

    if (vertexCount == 3) {
        l3 = l0;
    } else if (vertexCount == 4) {
        l3 = SafeNormalize(l3, vec3(0.0, 0.0, 1.0));
        l4 = l0;
    } else {
        l3 = SafeNormalize(l3, vec3(0.0, 0.0, 1.0));
        l4 = SafeNormalize(l4, vec3(0.0, 0.0, 1.0));
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

float HPWaterEvaluateAreaLightLTCPolygon(vec3 samplePos,
                                         mat3 basis,
                                         mat3 inverseLTC,
                                         vec3 p0,
                                         vec3 p1,
                                         vec3 p2,
                                         vec3 p3) {
    vec3 v0 = p0 - samplePos;
    vec3 v1 = p1 - samplePos;
    vec3 v2 = p2 - samplePos;
    vec3 v3 = p3 - samplePos;
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

vec3 ComputeHPWaterAreaLightRadiance(vec3 samplePos,
                                     vec3 N,
                                     vec3 sampleToCamera,
                                     float roughness,
                                     int lightIndex,
                                     out vec3 Lp) {
    if (u_HPWaterLightPayloadEnabled == 1 && lightIndex >= u_HPWaterAreaLightPayloadCount) {
        Lp = vec3(0.0, 1.0, 0.0);
        return vec3(0.0);
    }

    vec3 center = u_AreaLightPositions[lightIndex];
    vec3 right = SafeNormalize(u_AreaLightRights[lightIndex], vec3(1.0, 0.0, 0.0));
    vec3 up = SafeNormalize(u_AreaLightUps[lightIndex], vec3(0.0, 1.0, 0.0));
    vec3 forward = SafeNormalize(u_AreaLightForwards[lightIndex], vec3(0.0, 0.0, 1.0));
    vec3 color = u_AreaLightColors[lightIndex];
    float intensity = u_AreaLightIntensities[lightIndex];
    float width = max(u_AreaLightWidths[lightIndex], 0.001);
    float height = max(u_AreaLightHeights[lightIndex], 0.001);
    float lightRange = max(u_AreaLightRanges[lightIndex], 0.001);
    if (u_HPWaterLightPayloadEnabled == 1) {
        int base = lightIndex * 5;
        vec4 positionRange = u_HPWaterAreaLightPayload[base + 0];
        vec4 rightWidth = u_HPWaterAreaLightPayload[base + 1];
        vec4 upHeight = u_HPWaterAreaLightPayload[base + 2];
        vec4 forwardPayload = u_HPWaterAreaLightPayload[base + 3];
        vec4 colorIntensity = u_HPWaterAreaLightPayload[base + 4];
        center = positionRange.xyz;
        lightRange = max(positionRange.w, 0.001);
        right = SafeNormalize(rightWidth.xyz, vec3(1.0, 0.0, 0.0));
        width = max(rightWidth.w, 0.001);
        up = SafeNormalize(upHeight.xyz, vec3(0.0, 1.0, 0.0));
        height = max(upHeight.w, 0.001);
        forward = SafeNormalize(forwardPayload.xyz, vec3(0.0, 0.0, 1.0));
        color = colorIntensity.xyz;
        intensity = colorIntensity.w;
    }
    vec2 halfSize = vec2(width, height) * 0.5;
    vec3 centerVector = center - samplePos;
    float lightDistance = length(centerVector);
    if (lightDistance <= 0.0001 || lightDistance >= lightRange) {
        Lp = forward;
        return vec3(0.0);
    }

    Lp = centerVector / lightDistance;
    float emissionFacing = clamp(dot(forward, -Lp), 0.0, 1.0);
    if (emissionFacing <= 0.0001) {
        return vec3(0.0);
    }

    float nDotV = clamp(dot(N, sampleToCamera), 0.0, 1.0);
    vec4 ltc = SampleHPWaterAreaLightLTC(roughness, nDotV, 0);
    vec4 disneyLtc = SampleHPWaterAreaLightLTC(roughness, nDotV, 1);

    vec3 p0 = center - right * halfSize.x - up * halfSize.y;
    vec3 p1 = center + right * halfSize.x - up * halfSize.y;
    vec3 p2 = center + right * halfSize.x + up * halfSize.y;
    vec3 p3 = center - right * halfSize.x + up * halfSize.y;
    mat3 basis = HPWaterLTCViewNormalBasis(N, sampleToCamera);
    float specIrradiance = HPWaterEvaluateAreaLightLTCPolygon(
        samplePos, basis, HPWaterLTCInverseMatrix(ltc), p0, p1, p2, p3);
    float diffuseIrradiance = HPWaterEvaluateAreaLightLTCPolygon(
        samplePos, basis, HPWaterLTCInverseMatrix(disneyLtc), p0, p1, p2, p3);
    float projectedArea = width * height;
    float solidAngleFallback = clamp(projectedArea /
        max(lightDistance * lightDistance + projectedArea, 0.001), 0.0, 1.0);
    specIrradiance = max(specIrradiance, solidAngleFallback * 0.25);
    diffuseIrradiance = max(diffuseIrradiance, solidAngleFallback);

    float bodyScale = clamp(diffuseIrradiance / max(specIrradiance, 0.001), 0.0, 1.5);
    return color *
        max(intensity, 0.0) *
        HPWaterRangeAttenuation(lightDistance, lightRange) *
        emissionFacing * specIrradiance * bodyScale;
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

vec3 ComputeHPWaterPointLightScatter(int i,
                                     vec3 samplePos,
                                     vec3 sampleToCamera,
                                     vec3 scatteringAlbedo) {
    if (u_HPWaterLightPayloadEnabled == 1 && i >= u_HPWaterPointLightPayloadCount)
        return vec3(0.0);

    vec3 position = u_PointLightPositions[i];
    vec3 color = u_PointLightColors[i];
    float intensity = u_PointLightIntensities[i];
    float range = u_PointLightRanges[i];
    if (u_HPWaterLightPayloadEnabled == 1) {
        int base = i * 2;
        vec4 positionRange = u_HPWaterPointLightPayload[base + 0];
        vec4 colorIntensity = u_HPWaterPointLightPayload[base + 1];
        position = positionRange.xyz;
        range = positionRange.w;
        color = colorIntensity.xyz;
        intensity = colorIntensity.w;
    }

    vec3 lightVector = position - samplePos;
    float lightDistance = length(lightVector);
    float lightRange = max(range, 0.001);
    if (lightDistance <= 0.0001 || lightDistance >= lightRange)
        return vec3(0.0);

    vec3 Lp = lightVector / lightDistance;
    float attenuation = HPWaterRangeAttenuation(lightDistance, lightRange);
    float cosTheta = clamp(dot(sampleToCamera, Lp), -1.0, 1.0);
    vec3 phase = HPWaterEffectiveScatterPhase(cosTheta, scatteringAlbedo);
    vec3 radiance = color * max(intensity, 0.0) * attenuation;
    return radiance * phase * 2.2;
}

vec3 ComputeHPWaterSpotLightScatter(int i,
                                    vec3 samplePos,
                                    vec3 sampleToCamera,
                                    vec3 scatteringAlbedo) {
    if (u_HPWaterLightPayloadEnabled == 1 && i >= u_HPWaterSpotLightPayloadCount)
        return vec3(0.0);

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

    vec3 lightVector = position - samplePos;
    float lightDistance = length(lightVector);
    float lightRange = max(range, 0.001);
    if (lightDistance <= 0.0001 || lightDistance >= lightRange)
        return vec3(0.0);

    vec3 Lp = lightVector / lightDistance;
    vec3 spotForward = SafeNormalize(-direction, vec3(0.0, -1.0, 0.0));
    float theta = dot(Lp, spotForward);
    float epsilon = max(innerCos - outerCos, 0.001);
    float spotFactor = clamp((theta - outerCos) / epsilon, 0.0, 1.0);
    spotFactor *= spotFactor;
    if (spotFactor <= 0.0001)
        return vec3(0.0);

    float attenuation = HPWaterRangeAttenuation(lightDistance, lightRange) * spotFactor;
    float cosTheta = clamp(dot(sampleToCamera, Lp), -1.0, 1.0);
    vec3 phase = HPWaterEffectiveScatterPhase(cosTheta, scatteringAlbedo);
    vec3 radiance = color * max(intensity, 0.0) * attenuation;
    return radiance * phase * 2.2;
}

vec3 ComputeHPWaterAreaLightScatter(int i,
                                    vec3 samplePos,
                                    vec3 sampleToCamera,
                                    vec3 N,
                                    float roughness,
                                    vec3 scatteringAlbedo) {
    vec3 Lp = vec3(0.0, 1.0, 0.0);
    vec3 radiance = ComputeHPWaterAreaLightRadiance(samplePos,
                                                    N,
                                                    sampleToCamera,
                                                    roughness,
                                                    i,
                                                    Lp);
    if (max(max(radiance.r, radiance.g), radiance.b) <= 0.00001)
        return vec3(0.0);

    float cosTheta = clamp(dot(sampleToCamera, Lp), -1.0, 1.0);
    return radiance * HPWaterEffectiveScatterPhase(cosTheta, scatteringAlbedo) * 2.2;
}

vec3 ComputeHPWaterVolumePunctualLighting(vec3 samplePos,
                                          vec3 sampleToCamera,
                                          vec3 N,
                                          float roughness,
                                          vec3 scatteringAlbedo,
    float normalizedThickness) {
    vec3 punctual = vec3(0.0);
    int pointCount = u_HPWaterLightPayloadEnabled == 1
        ? max(u_HPWaterPointLightPayloadCount, 0)
        : clamp(u_NumPointLights, 0, 8);
    int spotCount = u_HPWaterLightPayloadEnabled == 1
        ? max(u_HPWaterSpotLightPayloadCount, 0)
        : clamp(u_NumSpotLights, 0, 4);
    int areaCount = u_HPWaterLightPayloadEnabled == 1
        ? max(u_HPWaterAreaLightPayloadCount, 0)
        : clamp(u_NumAreaLights, 0, 4);
    vec2 fullPixel = ResolveHPWaterFullResUV() * vec2(max(textureSize(u_HPWaterDepth, 0), ivec2(1)));
    int tiledReferenceOffset = 0;
    int tiledReferenceCount = 0;
    bool usedTiledLightList = HPWaterFetchTiledLightList(fullPixel,
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
                    punctual += ComputeHPWaterAreaLightScatter(lightIndex,
                                                               samplePos,
                                                               sampleToCamera,
                                                               N,
                                                               roughness,
                                                               scatteringAlbedo);
                }
            } else if ((referenceValue & HPWATER_LIGHT_REF_SPOT_FLAG) != 0u) {
                if (lightIndex < spotCount) {
                    punctual += ComputeHPWaterSpotLightScatter(lightIndex,
                                                               samplePos,
                                                               sampleToCamera,
                                                               scatteringAlbedo);
                }
            } else if (lightIndex < pointCount) {
                punctual += ComputeHPWaterPointLightScatter(lightIndex,
                                                            samplePos,
                                                            sampleToCamera,
                                                            scatteringAlbedo);
            }
        }
    } else {
        for (int i = 0; i < 8; ++i) {
            if (i >= pointCount)
                break;
            punctual += ComputeHPWaterPointLightScatter(i,
                                                        samplePos,
                                                        sampleToCamera,
                                                        scatteringAlbedo);
        }
        for (int i = 0; i < 4; ++i) {
            if (i >= spotCount)
                break;
            punctual += ComputeHPWaterSpotLightScatter(i,
                                                       samplePos,
                                                       sampleToCamera,
                                                       scatteringAlbedo);
        }
        for (int i = 0; i < 4; ++i) {
            if (i >= areaCount)
                break;
            punctual += ComputeHPWaterAreaLightScatter(i,
                                                       samplePos,
                                                       sampleToCamera,
                                                       N,
                                                       roughness,
                                                       scatteringAlbedo);
        }
    }

    return punctual * (0.55 + 0.45 * normalizedThickness);
}

vec3 ComputeHPWaterVolumeAreaLighting(vec3 samplePos,
                                      vec3 sampleToCamera,
                                      vec3 N,
                                      float roughness,
                                      vec3 scatteringAlbedo,
    float normalizedThickness) {
    vec3 areaScatter = vec3(0.0);
    int areaCount = u_HPWaterLightPayloadEnabled == 1
        ? max(u_HPWaterAreaLightPayloadCount, 0)
        : clamp(u_NumAreaLights, 0, 4);
    vec2 fullPixel = ResolveHPWaterFullResUV() * vec2(max(textureSize(u_HPWaterDepth, 0), ivec2(1)));
    int tiledReferenceOffset = 0;
    int tiledReferenceCount = 0;
    bool usedTiledLightList = HPWaterFetchTiledLightList(fullPixel,
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
            if ((referenceValue & HPWATER_LIGHT_REF_AREA_FLAG) == 0u) {
                continue;
            }

            int lightIndex = HPWaterDecodeLightReferenceIndex(referenceValue);
            if (lightIndex < areaCount) {
                areaScatter += ComputeHPWaterAreaLightScatter(lightIndex,
                                                              samplePos,
                                                              sampleToCamera,
                                                              N,
                                                              roughness,
                                                              scatteringAlbedo);
            }
        }
    } else {
        for (int i = 0; i < 4; ++i) {
            if (i >= areaCount)
                break;
            areaScatter += ComputeHPWaterAreaLightScatter(i,
                                                          samplePos,
                                                          sampleToCamera,
                                                          N,
                                                          roughness,
                                                          scatteringAlbedo);
        }
    }

    return areaScatter * (0.55 + 0.45 * normalizedThickness);
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
    vec2 sourceUV = ResolveHPWaterFullResUV();
    float waterDepth = texture(u_HPWaterDepth, sourceUV).r;
    vec4 refractData = texture(u_HPWaterRefractionWorldData, sourceUV);
    vec4 refractMeta = texture(u_HPWaterRefractionMeta, sourceUV);
    float waterMask = u_HPWaterMaskEnabled == 1
        ? texture(u_HPWaterMask, sourceUV).r
        : (waterDepth < 0.9999 ? 1.0 : 0.0);

    if (waterMask < 0.5 || waterDepth >= 0.9999) {
        VolumeColor = vec4(0.0);
        VolumeTransmittance = vec4(1.0, 1.0, 1.0, 0.0);
        VolumeDepth = vec4(0.0);
        VolumeAreaLightDiagnostics = vec4(0.0);
        return;
    }

    vec4 normalRoughness = texture(u_HPWaterNormalRoughness, sourceUV);
    vec4 scatterThickness = texture(u_HPWaterScatterThickness, sourceUV);
    vec4 absorptionFoam = texture(u_HPWaterAbsorptionFoam, sourceUV);

    vec3 N = DecodeHPWaterNormalRoughness(normalRoughness);
    vec3 scatterColor = max(scatterThickness.rgb, vec3(0.0));
    float depthTintDistance = max(scatterThickness.a, 0.1);
    vec3 absorptionColor = max(absorptionFoam.rgb, vec3(0.0001));
    float foam = clamp(absorptionFoam.a, 0.0, 1.0);

    vec3 waterWorldPos = ReconstructWorldPosition(sourceUV, waterDepth);
    vec3 refractedWorldPos = refractData.xyz;
    float rayLength = refractData.w;
    float normalizedThickness = clamp(refractMeta.w, 0.0, 1.0);
    vec3 V = SafeNormalize(u_CameraPosition - waterWorldPos, vec3(0.0, 1.0, 0.0));
    if (rayLength <= 0.001) {
        float fallbackThickness = normalizedThickness > 0.0001
            ? normalizedThickness
            : 0.35;
        rayLength = max(fallbackThickness * depthTintDistance, 0.05);
        refractedWorldPos = waterWorldPos - V * rayLength;
    }
    if (normalizedThickness <= 0.0001) {
        normalizedThickness = clamp(rayLength / max(depthTintDistance, 0.1), 0.05, 1.0);
    }

    vec3 L = normalize(u_LightDir);
    float NdotL = clamp(dot(N, L), 0.0, 1.0);

    vec3 absorptionCoeff = absorptionColor * 1.35;
    vec3 scatterCoeff = max(scatterColor, vec3(0.0001)) * 0.42;
    vec3 extinction = max(absorptionCoeff + scatterCoeff, vec3(0.0001));
    vec3 scatteringAlbedo = scatterCoeff / extinction;
    float macroScatter = clamp(u_MacroScatterStrength, 0.0, 4.0);
    vec3 fullRayVector = refractedWorldPos - waterWorldPos;
    float fullRayLength = max(rayLength, 0.0001);
    if (length(fullRayVector) <= 0.0001) {
        fullRayVector = SafeNormalize(refractedWorldPos - waterWorldPos, -V) * fullRayLength;
    }

    float maxCrossDistance = clamp(u_MaxRefractionCrossDistance, 0.1, 200.0);
    float noLinearScale = min(fullRayLength, maxCrossDistance) / fullRayLength;
    float noLinearRayLength = fullRayLength * noLinearScale;
    vec3 noLinearEndPos = waterWorldPos + fullRayVector * noLinearScale;
    float extinctionStrength = dot(extinction, vec3(0.2126, 0.7152, 0.0722));
    float distanceToQuarterEnergy = -log(0.25) / max(extinctionStrength, 0.000001);
    float dynamicShadowDistance = clamp(distanceToQuarterEnergy, 0.1, maxCrossDistance);
    float dynamicShadowScale = min(fullRayLength, dynamicShadowDistance) / fullRayLength;
    vec3 dynamicShadowEndPos = waterWorldPos + fullRayVector * dynamicShadowScale;
    float ambientDepth = abs(SafeNormalize(refractedWorldPos, vec3(0.0, -1.0, 0.0)).y) *
        noLinearRayLength;
    float sunDepth = ambientDepth * min(1.0 / max(abs(V.y), 0.001), 4.0);

    vec3 baseDirectLight = u_LightColor * max(u_LightIntensity, 0.0) *
        (0.18 + 0.82 * NdotL) * (0.75 + 0.25 * normalizedThickness);
    if (u_HPWaterCausticEnabled == 1) {
        vec4 caustic = texture(u_HPWaterCaustic, sourceUV);
        float causticWeight = clamp(caustic.a, 0.0, 1.0) *
            (0.25 + 0.75 * normalizedThickness);
        baseDirectLight += caustic.rgb * causticWeight * clamp(u_CausticVolumeStrength, 0.0, 4.0);
    }

    const float expFactor = 12.0;
    int sampleCount = clamp(u_VolumeSampleCount, 4, 32);
    vec2 volumeSize = max(u_HPWaterVolumeResolution.xy, vec2(1.0));
    float dither = InterleavedGradientNoise(gl_FragCoord.xy, u_FrameIndex);
    float invSampleCount = 1.0 / float(sampleCount);
    float previousD = 0.0;
    vec3 accumTransmittance = vec3(1.0);
    vec3 directScatter = vec3(0.0);
    vec3 sceneInScatter = vec3(0.0);
    vec3 areaScatterDiagnostics = vec3(0.0);
    float lastShadowVisibility = 1.0;
    float accumulatedDistance = 0.0;

    for (int i = 0; i < 32; ++i) {
        if (i >= sampleCount)
            break;

        float stepT = (float(i) + dither) * invSampleCount;
        float d = (pow(expFactor, stepT) - 1.0) / (expFactor - 1.0);
        if (i == sampleCount - 1) {
            d = 1.0;
        }

        float segment = max((d - previousD) * noLinearRayLength, 0.0);
        float midD = clamp((previousD + d) * 0.5, 0.0, 1.0);
        vec3 samplePos = mix(waterWorldPos, noLinearEndPos, midD);
        vec3 shadowSamplePos = mix(waterWorldPos, dynamicShadowEndPos, midD);
        vec3 sampleToCamera = SafeNormalize(u_CameraPosition - samplePos, V);
        float cosTheta = clamp(dot(sampleToCamera, L), -1.0, 1.0);
        vec3 phase = HPWaterEffectiveScatterPhase(cosTheta, scatteringAlbedo);
        float shadowVisibility = ComputeHPWaterVolumeShadow(shadowSamplePos, N);
        lastShadowVisibility = shadowVisibility;
        float volumeShadow = mix(0.35, 1.0, shadowVisibility);
        float directSegment = segment + max((d - previousD) * sunDepth, 0.0);
        vec3 stepTransmittance = exp(-extinction * segment);
        vec3 directStepTransmittance = exp(-extinction * directSegment);
        vec3 extinguished = vec3(1.0) - stepTransmittance;
        vec3 directExtinguished = vec3(1.0) - directStepTransmittance;
        float depthWeight = 0.55 + 0.45 * smoothstep(0.0, 1.0, midD);

        vec3 directionalScatter = baseDirectLight * volumeShadow * (phase * 2.6);
        vec3 punctualScatter = ComputeHPWaterVolumePunctualLighting(samplePos,
                                                                    sampleToCamera,
                                                                    N,
                                                                    clamp(normalRoughness.w, 0.02, 1.0),
                                                                    scatteringAlbedo,
                                                                    normalizedThickness);
        vec3 areaScatter = ComputeHPWaterVolumeAreaLighting(samplePos,
                                                            sampleToCamera,
                                                            N,
                                                            clamp(normalRoughness.w, 0.02, 1.0),
                                                            scatteringAlbedo,
                                                            normalizedThickness);
        directScatter += accumTransmittance *
            (directionalScatter * directExtinguished + punctualScatter * extinguished) *
            scatteringAlbedo * macroScatter * depthWeight;
        areaScatterDiagnostics += accumTransmittance *
            areaScatter * extinguished * scatteringAlbedo * macroScatter * depthWeight;

        accumTransmittance *= stepTransmittance;
        accumulatedDistance += segment;
        previousD = d;
    }

    vec3 transmittance = clamp(accumTransmittance, vec3(0.0), vec3(1.0));
    if (accumulatedDistance <= 0.0001) {
        transmittance = exp(-extinction * rayLength);
    }

    vec2 finalSceneUV = clamp(refractMeta.xy, vec2(0.001), vec2(0.999));
    vec3 finalSceneColor = texture(u_SceneColor, finalSceneUV).rgb;
    float lightLuminance = Luminance(u_LightColor * max(u_LightIntensity, 0.0));
    float sceneShadowMix = mix(lastShadowVisibility, 1.0, 0.3);
    sceneInScatter = finalSceneColor * sceneShadowMix * accumTransmittance *
        scatterCoeff * noLinearRayLength * lightLuminance * macroScatter;

    vec3 ambientScatter = scatterColor * (0.025 + 0.12 * normalizedThickness) * macroScatter;
    vec3 volumeColor = directScatter + sceneInScatter + ambientScatter;
    volumeColor = mix(volumeColor, vec3(0.96, 0.98, 1.0), foam * 0.35);

    float refractedLinearDepth = refractMeta.z >= 0.9999
        ? LinearizeDepth(waterDepth) + rayLength
        : LinearizeDepth(refractMeta.z);

    VolumeColor = vec4(max(volumeColor, vec3(0.0)), 1.0);
    VolumeTransmittance = vec4(clamp(transmittance, vec3(0.0), vec3(1.0)), 1.0);
    float ditherEvidence = clamp(dither, 0.001, 0.999);
    VolumeDepth = vec4(refractedLinearDepth, rayLength, normalizedThickness, ditherEvidence);
    VolumeAreaLightDiagnostics = vec4(
        max(areaScatterDiagnostics, vec3(0.0)),
        u_NumAreaLights > 0 ? 1.0 : 0.0);
}
#endif

            ENDGLSL
        }
    }

    FallBack "VibeEngine/HPWaterComposite"
}
