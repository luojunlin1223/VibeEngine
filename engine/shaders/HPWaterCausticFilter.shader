// VibeEngine ShaderLab - HPWater caustic denoise/filter pass
//
// HPWater filters caustic irradiance after compute accumulation. This OpenGL
// slice keeps the same dataflow contract by edge-aware filtering the caustic
// energy texture before water composite and volume lighting consume it.

Shader "VibeEngine/HPWaterCausticFilter" {
    Properties {
    }

    SubShader {
        Tags { "RenderType"="Opaque" "Queue"="Overlay" "LightMode"="HPWaterCausticFilter" }

        Pass {
            Name "HPWaterCausticFilterPass"

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

uniform sampler2D u_CausticInput;
uniform sampler2D u_HPWaterDepth;
uniform sampler2D u_HPWaterMask;

uniform float u_FilterStep;
uniform float u_FilterRadius;
uniform float u_DepthSigma;
uniform int u_HPWaterMaskEnabled;

void main() {
    float centerDepth = texture(u_HPWaterDepth, v_UV).r;
    float centerMask = u_HPWaterMaskEnabled == 1
        ? texture(u_HPWaterMask, v_UV).r
        : (centerDepth < 0.9999 ? 1.0 : 0.0);

    if (centerMask < 0.5 || centerDepth >= 0.9999) {
        FragColor = vec4(0.0);
        return;
    }

    ivec2 texSize = textureSize(u_CausticInput, 0);
    vec2 texel = 1.0 / vec2(max(texSize, ivec2(1)));
    float stepRadius = max(u_FilterStep, 1.0) * max(u_FilterRadius, 0.25);
    float depthSigma = max(u_DepthSigma, 0.00001);

    vec4 accum = vec4(0.0);
    float totalWeight = 0.0;

    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            vec2 offset = vec2(x, y);
            float r2 = dot(offset, offset);
            vec2 sampleUV = clamp(v_UV + offset * texel * stepRadius, vec2(0.001), vec2(0.999));
            vec4 sampleCaustic = texture(u_CausticInput, sampleUV);
            float sampleDepth = texture(u_HPWaterDepth, sampleUV).r;
            float sampleMask = u_HPWaterMaskEnabled == 1
                ? texture(u_HPWaterMask, sampleUV).r
                : (sampleDepth < 0.9999 ? 1.0 : 0.0);

            float spatialWeight = exp(-r2 * 0.42);
            float depthWeight = exp(-abs(sampleDepth - centerDepth) / depthSigma);
            float energyWeight = 0.25 + clamp(sampleCaustic.a, 0.0, 1.0) * 0.75;
            float w = spatialWeight * depthWeight * sampleMask * energyWeight;
            accum += sampleCaustic * w;
            totalWeight += w;
        }
    }

    vec4 filtered = totalWeight > 0.00001 ? accum / totalWeight : texture(u_CausticInput, v_UV);
    filtered.rgb = max(filtered.rgb, vec3(0.0));
    filtered.a = clamp(filtered.a, 0.0, 1.0);
    FragColor = filtered * centerMask;
}
#endif

            ENDGLSL
        }
    }

    FallBack "VibeEngine/HPWaterCaustic"
}
