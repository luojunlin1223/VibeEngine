// VibeEngine ShaderLab - HPWater low-resolution volume spatial filter
// First a-trous-style depth-aware filter for the HPWater volume accumulation.

Shader "VibeEngine/HPWaterVolumeFilter" {
    Properties {
    }

    SubShader {
        Tags { "RenderType"="Opaque" "Queue"="Overlay" "LightMode"="HPWaterVolumeFilter" }

        Pass {
            Name "HPWaterVolumeFilterPass"

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
layout(location = 0) out vec4 FilteredColor;
layout(location = 1) out vec4 FilteredTransmittance;
layout(location = 2) out vec4 FilteredDepth;

uniform sampler2D u_VolumeColor;
uniform sampler2D u_VolumeTransmittance;
uniform sampler2D u_VolumeDepth;
uniform float u_FilterStep;
uniform int u_SpatialDepthAwareEnabled;
uniform float u_SpatialDepthSensitivity;

float KernelWeight(int offset) {
    int a = abs(offset);
    if (a == 0) return 6.0;
    if (a == 1) return 4.0;
    return 1.0;
}

void main() {
    vec4 centerColor = texture(u_VolumeColor, v_UV);
    vec4 centerTransmittance = texture(u_VolumeTransmittance, v_UV);
    vec4 centerDepth = texture(u_VolumeDepth, v_UV);

    if (centerDepth.a <= 0.0001) {
        FilteredColor = centerColor;
        FilteredTransmittance = centerTransmittance;
        FilteredDepth = centerDepth;
        return;
    }

    ivec2 volumeSize = textureSize(u_VolumeColor, 0);
    ivec2 safeVolumeSize = max(volumeSize, ivec2(1));
    vec2 texel = 1.0 / vec2(safeVolumeSize);
    float stride = max(u_FilterStep, 1.0);

    vec3 colorAccum = vec3(0.0);
    vec3 transAccum = vec3(0.0);
    vec3 depthAccum = vec3(0.0);
    float totalWeight = 0.0;

    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            vec2 sampleUV = clamp(v_UV + vec2(x, y) * texel * stride, vec2(0.0), vec2(1.0));
            vec4 sampleColor = texture(u_VolumeColor, sampleUV);
            vec4 sampleTrans = texture(u_VolumeTransmittance, sampleUV);
            vec4 sampleDepth = texture(u_VolumeDepth, sampleUV);

            float valid = step(0.0001, sampleDepth.a);
            float spatialWeight = KernelWeight(x) * KernelWeight(y);
            float depthDelta = abs(sampleDepth.r - centerDepth.r);
            float rayDelta = abs(sampleDepth.g - centerDepth.g);
            float depthWeight = 1.0;
            if (u_SpatialDepthAwareEnabled == 1) {
                float sensitivity = max(u_SpatialDepthSensitivity, 0.0);
                depthWeight = exp(-depthDelta * sensitivity * 0.075 -
                    rayDelta * sensitivity * 0.0045);
            }
            float w = spatialWeight * depthWeight * valid;

            colorAccum += sampleColor.rgb * w;
            transAccum += sampleTrans.rgb * w;
            depthAccum += sampleDepth.rgb * w;
            totalWeight += w;
        }
    }

    if (totalWeight <= 0.00001) {
        FilteredColor = centerColor;
        FilteredTransmittance = centerTransmittance;
        FilteredDepth = centerDepth;
        return;
    }

    vec3 filteredColor = colorAccum / totalWeight;
    vec3 filteredTransmittance = clamp(transAccum / totalWeight, vec3(0.0), vec3(1.0));
    vec3 filteredDepth = depthAccum / totalWeight;

    FilteredColor = vec4(filteredColor, 1.0);
    FilteredTransmittance = vec4(filteredTransmittance, 1.0);
    FilteredDepth = vec4(filteredDepth, 1.0);
}
#endif

            ENDGLSL
        }
    }

    FallBack "VibeEngine/HPWaterVolume"
}
