// VibeEngine ShaderLab - HPWater explicit mask pass
// Builds a full-resolution water coverage texture from the dedicated HPWater
// depth buffer. This is the current OpenGL equivalent of HPWater's stencil
// isolation path and is shared by refraction, volume, and final composite.

Shader "VibeEngine/HPWaterMask" {
    Properties {
    }

    SubShader {
        Tags { "RenderType"="Opaque" "Queue"="Overlay" "LightMode"="HPWaterMask" }

        Pass {
            Name "HPWaterMaskPass"

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
layout(location = 0) out float Mask;

uniform sampler2D u_HPWaterDepth;

void main() {
    float waterDepth = texture(u_HPWaterDepth, v_UV).r;
    Mask = waterDepth < 0.9999 ? 1.0 : 0.0;
}
#endif

            ENDGLSL
        }
    }

    FallBack "VibeEngine/HPWaterComposite"
}
