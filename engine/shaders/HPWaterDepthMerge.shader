// VibeEngine ShaderLab - HPWater scene depth merge
// Writes dedicated HPWater surface depth into the main scene depth buffer.
// This mirrors HPWater/Unity's prepass behavior where water depth is visible
// to later screen-space passes without copying water payloads into the opaque
// G-buffer color attachments.

Shader "VibeEngine/HPWaterDepthMerge" {
    Properties {
    }

    SubShader {
        Tags { "RenderType"="Opaque" "Queue"="Overlay" "LightMode"="HPWaterDepthMerge" }

        Pass {
            Name "HPWaterDepthMergePass"

            Cull Off
            ZWrite On
            ZTest LEqual
            ColorMask 0

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

uniform sampler2D u_HPWaterDepth;

void main() {
    float waterDepth = texture(u_HPWaterDepth, v_UV).r;
    if (waterDepth >= 0.9999)
        discard;

    gl_FragDepth = waterDepth;
}
#endif

            ENDGLSL
        }
    }

    FallBack "VibeEngine/HPWaterMask"
}
