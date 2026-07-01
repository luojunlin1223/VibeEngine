// VibeEngine ShaderLab - HPWater caustic denoise/filter pass
//
// HPWater filters caustic irradiance after compute accumulation. This OpenGL
// slice mirrors HPWater's first 3x3 a-trous luminance edge filter, with the
// existing water mask/depth guard retained so deferred fullscreen filtering
// does not bleed outside VibeEngine's HPWater pixels.

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
uniform float u_LuminanceWeight;
uniform int u_HPWaterMaskEnabled;

float CausticLuminance(vec4 value) {
    return max(dot(max(value.rgb, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722)), max(value.a, 0.0));
}

float ComputeEdgeWeight(float centerLum, float sampleLum, float luminanceWeight) {
    float lumDiff = abs(log2(1.0 + max(centerLum, 0.0)) - log2(1.0 + max(sampleLum, 0.0)));
    float strictness = smoothstep(0.0, 0.25, min(centerLum, sampleLum));
    return exp2(-lumDiff * max(luminanceWeight, 0.0) * strictness);
}

float AtrousKernel3x3(int x, int y) {
    if (x == 0 && y == 0)
        return 4.0 / 16.0;
    if (x == 0 || y == 0)
        return 2.0 / 16.0;
    return 1.0 / 16.0;
}

float R2Dither(vec2 samplePosition) {
    const vec2 alpha = vec2(0.75487765, 0.56984026);
    return fract(dot(samplePosition, alpha));
}

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
    float r2Noise = R2Dither(v_UV * vec2(texSize));
    float angle = 6.2831853 * r2Noise * 0.125;
    float s = sin(angle);
    float c = cos(angle);
    mat2 rot = mat2(c, s, -s, c);
    stepRadius *= mix(0.875, 1.125, r2Noise);
    float depthSigma = max(u_DepthSigma, 0.00001);
    vec4 centerCaustic = texture(u_CausticInput, v_UV);
    float centerLum = CausticLuminance(centerCaustic);

    vec4 accum = vec4(0.0);
    float totalWeight = 0.0;

    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 offset = vec2(x, y);
            vec2 sampleUV = clamp(v_UV + (rot * offset) * texel * stepRadius, vec2(0.001), vec2(0.999));
            vec4 sampleCaustic = texture(u_CausticInput, sampleUV);
            float sampleDepth = texture(u_HPWaterDepth, sampleUV).r;
            float sampleMask = u_HPWaterMaskEnabled == 1
                ? texture(u_HPWaterMask, sampleUV).r
                : (sampleDepth < 0.9999 ? 1.0 : 0.0);

            float sampleLum = CausticLuminance(sampleCaustic);
            float spatialWeight = AtrousKernel3x3(x, y);
            float edgeWeight = ComputeEdgeWeight(centerLum, sampleLum, u_LuminanceWeight);
            float depthWeight = exp(-abs(sampleDepth - centerDepth) / depthSigma);
            float guardWeight = sampleMask * mix(depthWeight, 1.0, 0.35);
            float w = spatialWeight * mix(edgeWeight, 1.0, 0.25) * guardWeight;
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
