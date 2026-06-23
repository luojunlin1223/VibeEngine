// VibeEngine ShaderLab - HPWater full-resolution volume upsample
// Resolves filtered low-resolution volume color/transmittance/depth into
// full-resolution textures with a joint bilateral weight driven by the
// full-resolution refracted scene depth.

Shader "VibeEngine/HPWaterVolumeUpsample" {
    Properties {
    }

    SubShader {
        Tags { "RenderType"="Opaque" "Queue"="Overlay" "LightMode"="HPWaterVolumeUpsample" }

        Pass {
            Name "HPWaterVolumeUpsamplePass"

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
layout(location = 0) out vec4 UpsampledColor;
layout(location = 1) out vec4 UpsampledTransmittance;
layout(location = 2) out vec4 UpsampledDepth;

uniform sampler2D u_LowResVolumeColor;
uniform sampler2D u_LowResVolumeTransmittance;
uniform sampler2D u_LowResVolumeDepth;
uniform sampler2D u_HPWaterDepth;
uniform sampler2D u_HPWaterRefractionMeta;

uniform float u_NearClip;
uniform float u_FarClip;

float LinearizeDepth(float depth) {
    float z = depth * 2.0 - 1.0;
    return (2.0 * u_NearClip * u_FarClip) /
        max(u_FarClip + u_NearClip - z * (u_FarClip - u_NearClip), 0.0001);
}

float SpatialKernel(ivec2 offset) {
    int d2 = offset.x * offset.x + offset.y * offset.y;
    if (d2 == 0) return 1.0;
    if (d2 == 1) return 0.72;
    if (d2 == 2) return 0.52;
    if (d2 == 4) return 0.28;
    return 0.20;
}

void main() {
    float waterDepth = texture(u_HPWaterDepth, v_UV).r;
    vec4 refractMeta = texture(u_HPWaterRefractionMeta, v_UV);
    if (waterDepth >= 0.9999 || refractMeta.w <= 0.0001) {
        UpsampledColor = vec4(0.0);
        UpsampledTransmittance = vec4(1.0, 1.0, 1.0, 0.0);
        UpsampledDepth = vec4(0.0);
        return;
    }

    float waterLinear = LinearizeDepth(waterDepth);
    float targetDepth = refractMeta.z >= 0.9999
        ? waterLinear + max(refractMeta.w, 0.05) * 80.0
        : LinearizeDepth(refractMeta.z);

    ivec2 lowSize = textureSize(u_LowResVolumeColor, 0);
    vec2 lowTexel = 1.0 / vec2(max(lowSize, ivec2(1)));
    ivec2 center = ivec2(clamp(floor(v_UV * vec2(lowSize)), vec2(0.0), vec2(lowSize - ivec2(1))));

    vec3 colorAccum = vec3(0.0);
    vec3 transAccum = vec3(0.0);
    vec3 depthAccum = vec3(0.0);
    float totalWeight = 0.0;

    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            ivec2 offset = ivec2(x, y);
            ivec2 p = clamp(center + offset, ivec2(0), lowSize - ivec2(1));
            vec2 sampleUV = (vec2(p) + vec2(0.5)) * lowTexel;

            vec4 volumeColor = texture(u_LowResVolumeColor, sampleUV);
            vec4 volumeTrans = texture(u_LowResVolumeTransmittance, sampleUV);
            vec4 volumeDepth = texture(u_LowResVolumeDepth, sampleUV);

            float valid = step(0.0001, volumeDepth.a) * step(0.0001, volumeColor.a + volumeTrans.a);
            float depthDelta = abs(volumeDepth.r - targetDepth);
            float depthScale = max(0.35, targetDepth * 0.018);
            float depthWeight = exp(-depthDelta / depthScale);
            float rayWeight = exp(-abs(volumeDepth.b - refractMeta.w) * 3.0);
            float w = SpatialKernel(offset) * depthWeight * rayWeight * valid;

            colorAccum += volumeColor.rgb * w;
            transAccum += volumeTrans.rgb * w;
            depthAccum += volumeDepth.rgb * w;
            totalWeight += w;
        }
    }

    if (totalWeight <= 0.00001) {
        vec4 nearestColor = texture(u_LowResVolumeColor, v_UV);
        vec4 nearestTrans = texture(u_LowResVolumeTransmittance, v_UV);
        vec4 nearestDepth = texture(u_LowResVolumeDepth, v_UV);
        UpsampledColor = nearestColor;
        UpsampledTransmittance = nearestTrans;
        UpsampledDepth = nearestDepth;
        return;
    }

    vec3 color = colorAccum / totalWeight;
    vec3 trans = clamp(transAccum / totalWeight, vec3(0.0), vec3(1.0));
    vec3 depth = depthAccum / totalWeight;

    UpsampledColor = vec4(max(color, vec3(0.0)), 1.0);
    UpsampledTransmittance = vec4(trans, 1.0);
    UpsampledDepth = vec4(depth, 1.0);
}
#endif

            ENDGLSL
        }
    }

    FallBack "VibeEngine/HPWaterVolumeFilter"
}
