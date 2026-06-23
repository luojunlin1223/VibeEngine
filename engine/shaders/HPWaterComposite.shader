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

uniform sampler2D u_SceneColor;
uniform sampler2D u_SceneDepth;
uniform sampler2D u_HPWaterDepthPyramid;
uniform sampler2D u_HPWaterNormalRoughness;
uniform sampler2D u_HPWaterScatterThickness;
uniform sampler2D u_HPWaterAbsorptionFoam;
uniform sampler2D u_HPWaterDepth;
uniform sampler2D u_HPWaterVolumeColor;
uniform sampler2D u_HPWaterVolumeTransmittance;
uniform sampler2D u_HPWaterVolumeDepth;

uniform float u_NearClip;
uniform float u_FarClip;
uniform float u_RefractionStrength;
uniform int u_HPWaterVolumeEnabled;
uniform int u_HPWaterDepthPyramidEnabled;
uniform int u_HPWaterDepthPyramidMipCount;
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

vec2 FindRefractedUV(vec2 uv, vec2 direction, float waterLinearDepth, float sceneLinearDepth, float depthTintDistance) {
    float directionLength = length(direction);
    if (directionLength < 0.00001) {
        return uv;
    }

    vec2 rayDirection = direction / directionLength;
    float maxTravel = clamp(directionLength, 0.0, 0.16) * ScreenEdgeFade(uv);
    if (maxTravel <= 0.00001) {
        return uv;
    }

    const int sampleCount = 16;
    const float expFactor = 8.0;
    float maxCrossDistance = clamp(depthTintDistance, 0.5, 80.0);
    float hitTolerance = max(0.25, maxCrossDistance * 0.025);
    vec2 bestUV = clamp(uv + rayDirection * maxTravel, vec2(0.001), vec2(0.999));
    bool hit = false;
    float previousD = 0.0;

    for (int i = 1; i <= sampleCount; ++i) {
        float linearStep = float(i) / float(sampleCount);
        float d = (pow(expFactor, linearStep) - 1.0) / (expFactor - 1.0);
        vec2 sampleUV = clamp(uv + rayDirection * maxTravel * d, vec2(0.001), vec2(0.999));
        float sampleDepth = SampleSceneDepth(sampleUV, DepthPyramidLOD(d, maxTravel));

        if (sampleDepth >= 0.9999) {
            previousD = d;
            continue;
        }

        float sampleLinear = LinearizeDepth(sampleDepth);
        float rayLinear = waterLinearDepth + maxCrossDistance * d;

        if (sampleLinear <= waterLinearDepth + 0.02) {
            previousD = d;
            continue;
        }

        if (rayLinear >= sampleLinear - hitTolerance) {
            float lo = previousD;
            float hi = d;
            for (int refine = 0; refine < 5; ++refine) {
                float mid = (lo + hi) * 0.5;
                vec2 refineUV = clamp(uv + rayDirection * maxTravel * mid, vec2(0.001), vec2(0.999));
                float fineDepth = SampleSceneDepth(refineUV, 0.0);
                float fineLinear = LinearizeDepth(fineDepth);
                float fineRayLinear = waterLinearDepth + maxCrossDistance * mid;
                if (fineDepth < 0.9999 &&
                    fineLinear > waterLinearDepth + 0.02 &&
                    fineRayLinear >= fineLinear - hitTolerance) {
                    hi = mid;
                } else {
                    lo = mid;
                }
            }
            bestUV = clamp(uv + rayDirection * maxTravel * hi, vec2(0.001), vec2(0.999));
            hit = true;
            break;
        }

        previousD = d;
    }

    if (!hit && sceneLinearDepth <= waterLinearDepth + 0.02) {
        return uv;
    }

    return bestUV;
}

struct VolumeSample {
    vec3 color;
    vec3 transmittance;
    float weight;
};

VolumeSample SampleHPWaterVolume(vec2 uv, float sceneLinearDepth) {
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

    if (waterDepth >= 0.9999) {
        FragColor = sceneColor;
        RefractData = vec4(0.0);
        RefractMeta = vec4(v_UV, 1.0, 0.0);
        return;
    }

    float sceneDepth = texture(u_SceneDepth, v_UV).r;

    // Preserve foreground opaque objects. GL depth is smaller when closer.
    if (sceneDepth < waterDepth - 0.00005) {
        FragColor = sceneColor;
        RefractData = vec4(0.0);
        RefractMeta = vec4(v_UV, sceneDepth, 0.0);
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

    // HPWater-style refraction slice: march a normal-driven screen ray against
    // the opaque scene-depth pyramid, then refine the hit at mip 0.
    vec2 distortion = N.xz * (0.018 + 0.032 * (1.0 - roughness)) *
        clamp(u_RefractionStrength, 0.0, 2.0) *
        (0.25 + 0.75 * normalizedThickness);
    vec2 refractUV = FindRefractedUV(v_UV, distortion, waterLinear, sceneLinear, depthTintDistance);

    float refractedSceneDepth = SampleSceneDepth(refractUV, 0.0);
    if (refractedSceneDepth < waterDepth - 0.00005) {
        refractUV = v_UV;
        refractedSceneDepth = sceneDepth;
    }

    vec3 refractedColor = texture(u_SceneColor, refractUV).rgb;
    vec3 waterWorldPos = ReconstructWorldPosition(v_UV, waterDepth);
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

    float fresnel = pow(clamp(1.0 - max(N.y, 0.0), 0.0, 1.0), 2.0);
    vec3 skyTint = mix(scatterColor, vec3(0.78, 0.88, 0.98), 0.55);
    vec3 reflected = skyTint * (0.08 + 0.40 * fresnel);

    vec3 foamColor = mix(vec3(0.88, 0.94, 0.98), vec3(1.0), foam);
    vec3 waterColor = mix(bodyColor + reflected, foamColor, foam * 0.65);
    float waterAlpha = clamp(0.28 + normalizedThickness * 0.62 + foam * 0.45, 0.0, 0.92);

    FragColor = vec4(mix(sceneColor.rgb, waterColor, waterAlpha), sceneColor.a);
    RefractData = vec4(refractedWorldPos, rayLength);
    RefractMeta = vec4(refractUV, refractedSceneDepth, normalizedThickness);
}
#endif

            ENDGLSL
        }
    }

    FallBack "VibeEngine/DeferredLighting"
}
