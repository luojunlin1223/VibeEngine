// VibeEngine ShaderLab - HPWater screen-space reflection lighting buffer.
// Matches HPWater/HDRP's _SsrLightingTexture contract: rgb stores weighted
// reflection lighting and alpha stores the consumed reflection hierarchy weight.

Shader "VibeEngine/HPWaterSSR" {
    Properties {
    }

    SubShader {
        Tags { "RenderType"="Opaque" "Queue"="Overlay" "LightMode"="HPWaterSSR" }

        Pass {
            Name "HPWaterSSRPass"

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

uniform sampler2D u_SceneColor;
uniform sampler2D u_SceneDepth;
uniform sampler2D u_HPWaterDepthPyramid;
uniform sampler2D u_HPWaterNormalRoughness;
uniform sampler2D u_HPWaterDepth;
uniform sampler2D u_HPWaterMask;

uniform float u_NearClip;
uniform float u_FarClip;
uniform float u_HPWaterSSRStepSize;
uniform float u_HPWaterSSRThickness;
uniform float u_HPWaterSSRMaxDistance;
uniform float u_HPWaterSSRStrength;
uniform vec3 u_ViewPos;
uniform int u_HPWaterSSREnabled;
uniform int u_HPWaterSSRMaxSteps;
uniform int u_HPWaterDepthPyramidEnabled;
uniform int u_HPWaterDepthPyramidMipCount;
uniform int u_SceneColorMipEnabled;
uniform int u_SceneColorMipCount;
uniform int u_HPWaterMaskEnabled;
uniform mat4 u_ViewProjection;
uniform mat4 u_InverseViewProjection;

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

float ScreenEdgeFade(vec2 uv) {
    vec2 edge = min(uv, vec2(1.0) - uv);
    return clamp(min(edge.x, edge.y) * 8.0, 0.0, 1.0);
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

bool ProjectSSRRay(vec3 rayPos, out vec2 uv, out float rayLinearDepth) {
    vec4 clip = u_ViewProjection * vec4(rayPos, 1.0);
    if (clip.w <= 0.00001) {
        return false;
    }

    vec3 ndc = clip.xyz / clip.w;
    uv = ndc.xy * 0.5 + 0.5;
    if (uv.x <= 0.001 || uv.y <= 0.001 || uv.x >= 0.999 || uv.y >= 0.999 ||
        ndc.z <= -1.0 || ndc.z >= 1.0) {
        return false;
    }

    float rayDepth = ndc.z * 0.5 + 0.5;
    rayLinearDepth = LinearizeDepth(clamp(rayDepth, 0.0, 1.0));
    return true;
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
    float lastTravelled = 0.0;

    for (int i = 0; i < 128; ++i) {
        if (i >= maxSteps || travelled > maxDistance) {
            break;
        }

        float progress = float(i) / float(max(maxSteps - 1, 1));
        vec3 rayPos = start + R * travelled;
        vec2 uv;
        float rayLinear;
        if (!ProjectSSRRay(rayPos, uv, rayLinear)) {
            break;
        }

        if (rayLinear <= waterLinearDepth + 0.01) {
            lastTravelled = travelled;
            travelled += stepSize * mix(1.0, 2.25, progress);
            continue;
        }

        float sceneDepth = SampleSceneDepth(uv, DepthPyramidLOD(progress, maxDistance));
        if (sceneDepth < 0.9999) {
            float sceneLinear = LinearizeDepth(sceneDepth);
            float delta = rayLinear - sceneLinear;
            float adaptiveThickness = thickness + travelled * 0.015;
            if (delta >= -adaptiveThickness * 0.25 && delta <= adaptiveThickness) {
                vec2 hitUV = uv;
                float hitTravelled = travelled;
                float low = max(lastTravelled, 0.0);
                float high = travelled;
                for (int refine = 0; refine < 6; ++refine) {
                    float mid = (low + high) * 0.5;
                    vec2 refinedUV;
                    float refinedRayLinear;
                    if (!ProjectSSRRay(start + R * mid, refinedUV, refinedRayLinear)) {
                        low = mid;
                        continue;
                    }

                    float refinedSceneDepth = SampleSceneDepth(refinedUV, 0.0);
                    if (refinedSceneDepth >= 0.9999 ||
                        refinedRayLinear <= waterLinearDepth + 0.01) {
                        low = mid;
                        continue;
                    }

                    float refinedSceneLinear = LinearizeDepth(refinedSceneDepth);
                    float refinedAdaptiveThickness = thickness + mid * 0.015;
                    float refinedDelta = refinedRayLinear - refinedSceneLinear;
                    if (refinedDelta >= -refinedAdaptiveThickness * 0.25 &&
                        refinedDelta <= refinedAdaptiveThickness) {
                        high = mid;
                        hitTravelled = mid;
                        hitUV = refinedUV;
                    } else {
                        low = mid;
                    }
                }

                float edgeFade = ScreenEdgeFade(hitUV);
                float distanceFade = 1.0 - smoothstep(maxDistance * 0.35, maxDistance, hitTravelled);
                float roughnessFade = mix(1.0, 0.35, clamp(roughness, 0.0, 1.0));
                float confidence = edgeFade * distanceFade * roughnessFade *
                    clamp(u_HPWaterSSRStrength, 0.0, 1.0);
                float lod = roughness * float(max(u_SceneColorMipCount - 1, 0));
                vec3 color = SampleSceneColorBlurred(hitUV, lod) * confidence;
                return vec4(color, confidence);
            }
        }

        lastTravelled = travelled;
        travelled += stepSize * mix(1.0, 2.25, progress);
    }

    return vec4(0.0);
}

void main() {
    float waterDepth = texture(u_HPWaterDepth, v_UV).r;
    if (waterDepth >= 0.9999) {
        FragColor = vec4(0.0);
        return;
    }

    if (u_HPWaterMaskEnabled == 1 && texture(u_HPWaterMask, v_UV).r <= 0.001) {
        FragColor = vec4(0.0);
        return;
    }

    vec4 normalRoughness = texture(u_HPWaterNormalRoughness, v_UV);
    vec3 N = NormalizeOr(normalRoughness.xyz * 2.0 - 1.0, vec3(0.0, 1.0, 0.0));
    float roughness = clamp(normalRoughness.a, 0.02, 1.0);
    vec3 waterWorldPos = ReconstructWorldPosition(v_UV, waterDepth);
    vec3 V = NormalizeOr(u_ViewPos - waterWorldPos, vec3(0.0, 0.0, 1.0));
    float waterLinear = LinearizeDepth(waterDepth);
    FragColor = TraceHPWaterSSR(waterWorldPos, N, V, roughness, waterLinear);
}
#endif

            ENDGLSL
        }
    }

    FallBack "VibeEngine/DeferredLighting"
}
