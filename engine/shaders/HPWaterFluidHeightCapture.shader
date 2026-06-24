// VibeEngine ShaderLab - HPWater top-down fluid height capture
// Captures normalized world height into an R16F texture for FluidDynamics.

Shader "VibeEngine/HPWaterFluidHeightCapture" {
    Properties {
    }

    SubShader {
        Tags { "RenderType"="Opaque" "Queue"="Geometry" "LightMode"="HPWaterFluidHeightCapture" }

        Pass {
            Name "HPWaterFluidHeightCapturePass"

            Cull Off
            ZWrite On
            ZTest Less

            GLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag

#version 460 core

#ifdef VERTEX
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec3 a_Color;
layout(location = 3) in vec2 a_TexCoord;

uniform mat4 u_TopDownMVP;
uniform mat4 u_Model;
uniform vec3 u_BoxCenter;
uniform vec3 u_BoxSize;
uniform float u_ForceHeight;

out float v_NormalizedHeight;

void main() {
    vec3 worldPos = vec3(u_Model * vec4(a_Position, 1.0));
    float minY = u_BoxCenter.y - u_BoxSize.y * 0.5;
    v_NormalizedHeight = clamp((worldPos.y - minY) / max(u_BoxSize.y, 0.0001), 0.0, 1.0);
    gl_Position = u_TopDownMVP * vec4(a_Position, 1.0);
}
#endif

#ifdef FRAGMENT
in float v_NormalizedHeight;

uniform float u_ForceHeight;

layout(location = 0) out float gHeight;

void main() {
    gHeight = u_ForceHeight >= 0.0 ? clamp(u_ForceHeight, 0.0, 1.0) : v_NormalizedHeight;
}
#endif

            ENDGLSL
        }
    }

    FallBack "VibeEngine/HPWaterGBuffer"
}
