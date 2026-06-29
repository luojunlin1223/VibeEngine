// VibeEngine ShaderLab - HPWater full-resolution volume upsample
// Resolves filtered low-resolution volume color/transmittance/depth into
// full-resolution textures with HPWater-style 2x2 gather reconstruction:
// bilinear weights multiplied by reciprocal low/full-depth difference.

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
uniform sampler2D u_HPWaterMask;

uniform float u_NearClip;
uniform float u_FarClip;
uniform int u_HPWaterMaskEnabled;

float LinearizeDepth(float depth) {
    float z = depth * 2.0 - 1.0;
    return (2.0 * u_NearClip * u_FarClip) /
        max(u_FarClip + u_NearClip - z * (u_FarClip - u_NearClip), 0.0001);
}

struct VolumeTap {
    vec3 color;
    vec3 transmittance;
    vec3 depth;
    float valid;
};

VolumeTap LoadVolumeTap(ivec2 p) {
    VolumeTap tap;
    vec4 c = texelFetch(u_LowResVolumeColor, p, 0);
    vec4 t = texelFetch(u_LowResVolumeTransmittance, p, 0);
    vec4 d = texelFetch(u_LowResVolumeDepth, p, 0);
    tap.color = c.rgb;
    tap.transmittance = t.rgb;
    tap.depth = d.rgb;
    tap.valid = step(0.0001, d.a) * step(0.0001, c.a + t.a);
    return tap;
}

void main() {
    float waterDepth = texture(u_HPWaterDepth, v_UV).r;
    vec4 refractMeta = texture(u_HPWaterRefractionMeta, v_UV);
    float waterMask = u_HPWaterMaskEnabled == 1
        ? texture(u_HPWaterMask, v_UV).r
        : (waterDepth < 0.9999 ? 1.0 : 0.0);
    if (waterMask < 0.5 || waterDepth >= 0.9999 || refractMeta.w <= 0.0001) {
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
    vec2 lowResPixel = v_UV * vec2(lowSize);
    vec2 lowResCoord = lowResPixel - vec2(0.5);
    ivec2 p00 = ivec2(floor(lowResCoord));
    vec2 f = fract(lowResCoord);
    ivec2 p10 = p00 + ivec2(1, 0);
    ivec2 p01 = p00 + ivec2(0, 1);
    ivec2 p11 = p00 + ivec2(1, 1);
    ivec2 lowMax = lowSize - ivec2(1);
    p00 = clamp(p00, ivec2(0), lowMax);
    p10 = clamp(p10, ivec2(0), lowMax);
    p01 = clamp(p01, ivec2(0), lowMax);
    p11 = clamp(p11, ivec2(0), lowMax);

    VolumeTap tap00 = LoadVolumeTap(p00);
    VolumeTap tap10 = LoadVolumeTap(p10);
    VolumeTap tap01 = LoadVolumeTap(p01);
    VolumeTap tap11 = LoadVolumeTap(p11);

    float wb00 = (1.0 - f.x) * (1.0 - f.y);
    float wb10 = f.x * (1.0 - f.y);
    float wb01 = (1.0 - f.x) * f.y;
    float wb11 = f.x * f.y;

    float epsilon = 0.000001;
    float w00 = wb00 * tap00.valid / (abs(tap00.depth.r - targetDepth) + epsilon);
    float w10 = wb10 * tap10.valid / (abs(tap10.depth.r - targetDepth) + epsilon);
    float w01 = wb01 * tap01.valid / (abs(tap01.depth.r - targetDepth) + epsilon);
    float w11 = wb11 * tap11.valid / (abs(tap11.depth.r - targetDepth) + epsilon);
    float totalWeight = w00 + w10 + w01 + w11;

    if (totalWeight <= 0.00001) {
        vec4 nearestColor = texture(u_LowResVolumeColor, v_UV);
        vec4 nearestTrans = texture(u_LowResVolumeTransmittance, v_UV);
        vec4 nearestDepth = texture(u_LowResVolumeDepth, v_UV);
        UpsampledColor = nearestColor;
        UpsampledTransmittance = nearestTrans;
        UpsampledDepth = nearestDepth;
        return;
    }

    vec3 color = (tap00.color * w00 + tap10.color * w10 +
        tap01.color * w01 + tap11.color * w11) / totalWeight;
    vec3 trans = clamp((tap00.transmittance * w00 + tap10.transmittance * w10 +
        tap01.transmittance * w01 + tap11.transmittance * w11) / totalWeight,
        vec3(0.0), vec3(1.0));
    vec3 depth = (tap00.depth * w00 + tap10.depth * w10 +
        tap01.depth * w01 + tap11.depth * w11) / totalWeight;

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
