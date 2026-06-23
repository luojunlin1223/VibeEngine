// VibeEngine ShaderLab - HPWater low-resolution volume temporal filter
// Reprojects the previous low-resolution volume history through current and
// previous view-projection matrices, then blends with depth and velocity
// rejection before the a-trous spatial filter.

Shader "VibeEngine/HPWaterVolumeTemporal" {
    Properties {
    }

    SubShader {
        Tags { "RenderType"="Opaque" "Queue"="Overlay" "LightMode"="HPWaterVolumeTemporal" }

        Pass {
            Name "HPWaterVolumeTemporalPass"

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
layout(location = 0) out vec4 TemporalColor;
layout(location = 1) out vec4 TemporalTransmittance;
layout(location = 2) out vec4 TemporalDepth;

uniform sampler2D u_CurrentColor;
uniform sampler2D u_CurrentTransmittance;
uniform sampler2D u_CurrentDepth;
uniform sampler2D u_HistoryColor;
uniform sampler2D u_HistoryTransmittance;
uniform sampler2D u_HistoryDepth;
uniform sampler2D u_HPWaterRefractionWorldData;

uniform mat4 u_CurrentViewProjection;
uniform mat4 u_PreviousViewProjection;
uniform int u_HistoryValid;
uniform float u_HistoryBlend;
uniform float u_DepthRejectionThreshold;
uniform float u_VelocityRejectionScale;

float Saturate(float v) {
    return clamp(v, 0.0, 1.0);
}

vec2 ReprojectHistoryUV(vec3 worldPos, out bool valid) {
    vec4 prevClip = u_PreviousViewProjection * vec4(worldPos, 1.0);
    if (abs(prevClip.w) <= 0.00001) {
        valid = false;
        return v_UV;
    }

    vec3 prevNdc = prevClip.xyz / prevClip.w;
    vec2 historyUV = prevNdc.xy * 0.5 + 0.5;
    valid = prevNdc.z >= -1.0 && prevNdc.z <= 1.0 &&
        all(greaterThanEqual(historyUV, vec2(0.0))) &&
        all(lessThanEqual(historyUV, vec2(1.0)));
    return historyUV;
}

void main() {
    vec4 currentColor = texture(u_CurrentColor, v_UV);
    vec4 currentTransmittance = texture(u_CurrentTransmittance, v_UV);
    vec4 currentDepth = texture(u_CurrentDepth, v_UV);

    if (currentDepth.a <= 0.0001 || u_HistoryValid == 0) {
        TemporalColor = currentColor;
        TemporalTransmittance = currentTransmittance;
        TemporalDepth = currentDepth;
        return;
    }

    vec4 refractWorld = texture(u_HPWaterRefractionWorldData, v_UV);
    bool reprojectValid = false;
    vec2 historyUV = ReprojectHistoryUV(refractWorld.xyz, reprojectValid);
    if (!reprojectValid) {
        TemporalColor = currentColor;
        TemporalTransmittance = currentTransmittance;
        TemporalDepth = currentDepth;
        return;
    }

    vec4 historyColor = texture(u_HistoryColor, historyUV);
    vec4 historyTransmittance = texture(u_HistoryTransmittance, historyUV);
    vec4 historyDepth = texture(u_HistoryDepth, historyUV);
    if (historyDepth.a <= 0.0001) {
        TemporalColor = currentColor;
        TemporalTransmittance = currentTransmittance;
        TemporalDepth = currentDepth;
        return;
    }

    float depthThreshold = max(u_DepthRejectionThreshold, 0.0001) * max(currentDepth.r, 1.0);
    float depthDiff = abs(currentDepth.r - historyDepth.r);
    float depthWeight = 1.0 - smoothstep(depthThreshold * 0.35, depthThreshold, depthDiff);

    vec2 velocityPixels = (historyUV - v_UV) * vec2(textureSize(u_CurrentColor, 0));
    float velocity = length(velocityPixels);
    float velocityWeight = exp(-velocity * max(u_VelocityRejectionScale, 0.0) * 0.01);

    float historyWeight = Saturate(u_HistoryBlend) * depthWeight * velocityWeight;
    vec3 blendedColor = mix(currentColor.rgb, historyColor.rgb, historyWeight);
    vec3 blendedTransmittance = mix(currentTransmittance.rgb, historyTransmittance.rgb, historyWeight);
    vec3 blendedDepth = mix(currentDepth.rgb, historyDepth.rgb, historyWeight * 0.5);

    TemporalColor = vec4(max(blendedColor, vec3(0.0)), 1.0);
    TemporalTransmittance = vec4(clamp(blendedTransmittance, vec3(0.0), vec3(1.0)), 1.0);
    TemporalDepth = vec4(blendedDepth, 1.0);
}
#endif

            ENDGLSL
        }
    }

    FallBack "VibeEngine/HPWaterVolumeFilter"
}
