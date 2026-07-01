// VibeEngine ShaderLab - HPWater scene normal merge
// Writes dedicated HPWater normal/roughness into the main scene G-buffer RT1.
// HPWater's Unity path writes water normals into the prepass normal buffer so
// later screen-space effects see the water surface instead of hidden opaque data.

Shader "VibeEngine/HPWaterNormalMerge" {
    Properties {
    }

    SubShader {
        Tags { "RenderType"="Opaque" "Queue"="Overlay" "LightMode"="HPWaterNormalMerge" }

        Pass {
            Name "HPWaterNormalMergePass"

            Cull Off
            ZWrite Off
            ZTest LEqual

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
layout(location = 0) out vec4 OutNormalRoughness;

uniform sampler2D u_HPWaterDepth;
uniform sampler2D u_HPWaterNormalRoughness;

#include "hpwater_normal.glslinc"

void main() {
    float waterDepth = texture(u_HPWaterDepth, v_UV).r;
    if (waterDepth >= 0.9999)
        discard;

    gl_FragDepth = waterDepth;
    vec4 normalRoughness = texture(u_HPWaterNormalRoughness, v_UV);
    OutNormalRoughness = vec4(DecodeHPWaterNormalRoughness(normalRoughness), normalRoughness.a);
}
#endif

            ENDGLSL
        }
    }

    FallBack "VibeEngine/HPWaterDepthMerge"
}
