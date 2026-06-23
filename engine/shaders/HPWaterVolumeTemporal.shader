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
uniform sampler2D u_VolumeMotionVector;

uniform mat4 u_CurrentViewProjection;
uniform mat4 u_PreviousViewProjection;
uniform int u_HistoryValid;
uniform int u_MotionVectorValid;
uniform float u_HistoryBlend;
uniform float u_DepthRejectionThreshold;
uniform float u_VelocityRejectionScale;
uniform float u_NeighborhoodClampStrength;

struct NeighborhoodBounds {
    vec3 colorMin;
    vec3 colorMax;
    vec3 transmittanceMin;
    vec3 transmittanceMax;
    float depthMin;
    float depthMax;
};

float Saturate(float v) {
    return clamp(v, 0.0, 1.0);
}

vec2 ProjectUV(mat4 viewProjection, vec3 worldPos, out bool valid) {
    vec4 clip = viewProjection * vec4(worldPos, 1.0);
    if (abs(clip.w) <= 0.00001) {
        valid = false;
        return v_UV;
    }

    vec3 ndc = clip.xyz / clip.w;
    vec2 projectedUV = ndc.xy * 0.5 + 0.5;
    valid = ndc.z >= -1.0 && ndc.z <= 1.0 &&
        all(greaterThanEqual(projectedUV, vec2(0.0))) &&
        all(lessThanEqual(projectedUV, vec2(1.0)));
    return projectedUV;
}

NeighborhoodBounds GatherCurrentNeighborhood(vec2 uv) {
    ivec2 size = max(textureSize(u_CurrentColor, 0), ivec2(1));
    vec2 texel = 1.0 / vec2(size);

    vec4 centerColor = texture(u_CurrentColor, uv);
    vec4 centerTransmittance = texture(u_CurrentTransmittance, uv);
    vec4 centerDepth = texture(u_CurrentDepth, uv);

    NeighborhoodBounds bounds;
    bounds.colorMin = centerColor.rgb;
    bounds.colorMax = centerColor.rgb;
    bounds.transmittanceMin = centerTransmittance.rgb;
    bounds.transmittanceMax = centerTransmittance.rgb;
    bounds.depthMin = centerDepth.r;
    bounds.depthMax = centerDepth.r;

    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 sampleUV = clamp(uv + vec2(x, y) * texel, vec2(0.0), vec2(1.0));
            vec4 sampleColor = texture(u_CurrentColor, sampleUV);
            vec4 sampleTransmittance = texture(u_CurrentTransmittance, sampleUV);
            vec4 sampleDepth = texture(u_CurrentDepth, sampleUV);
            if (sampleDepth.a <= 0.0001) {
                continue;
            }

            bounds.colorMin = min(bounds.colorMin, sampleColor.rgb);
            bounds.colorMax = max(bounds.colorMax, sampleColor.rgb);
            bounds.transmittanceMin = min(bounds.transmittanceMin, sampleTransmittance.rgb);
            bounds.transmittanceMax = max(bounds.transmittanceMax, sampleTransmittance.rgb);
            bounds.depthMin = min(bounds.depthMin, sampleDepth.r);
            bounds.depthMax = max(bounds.depthMax, sampleDepth.r);
        }
    }

    return bounds;
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
    vec2 historyUV = ProjectUV(u_PreviousViewProjection, refractWorld.xyz, reprojectValid);
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

    NeighborhoodBounds bounds = GatherCurrentNeighborhood(v_UV);
    float clampStrength = Saturate(u_NeighborhoodClampStrength);
    vec3 colorPad = max((bounds.colorMax - bounds.colorMin) * 0.15, vec3(0.002));
    vec3 transPad = max((bounds.transmittanceMax - bounds.transmittanceMin) * 0.15, vec3(0.002));
    vec3 clampedHistoryColor = clamp(historyColor.rgb, bounds.colorMin - colorPad, bounds.colorMax + colorPad);
    vec3 clampedHistoryTransmittance = clamp(historyTransmittance.rgb,
        bounds.transmittanceMin - transPad,
        bounds.transmittanceMax + transPad);
    historyColor.rgb = mix(historyColor.rgb, clampedHistoryColor, clampStrength);
    historyTransmittance.rgb = mix(historyTransmittance.rgb, clampedHistoryTransmittance, clampStrength);

    float depthThreshold = max(u_DepthRejectionThreshold, 0.0001) * max(currentDepth.r, 1.0);
    float depthDiff = abs(currentDepth.r - historyDepth.r);
    float depthWeight = 1.0 - smoothstep(depthThreshold * 0.35, depthThreshold, depthDiff);
    float depthBoundsDiff = max(bounds.depthMin - historyDepth.r, historyDepth.r - bounds.depthMax);
    float depthBoundsWeight = 1.0 - smoothstep(depthThreshold * 0.25, depthThreshold, max(depthBoundsDiff, 0.0));
    depthWeight *= depthBoundsWeight;

    bool currentProjectValid = false;
    vec2 currentUV = ProjectUV(u_CurrentViewProjection, refractWorld.xyz, currentProjectValid);
    if (!currentProjectValid) {
        currentUV = v_UV;
    }

    vec2 motionVectorUV = u_MotionVectorValid == 1
        ? texture(u_VolumeMotionVector, v_UV).rg
        : (historyUV - currentUV);
    vec2 velocityPixels = motionVectorUV * vec2(textureSize(u_CurrentColor, 0));
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
