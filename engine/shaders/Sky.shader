// VibeEngine ShaderLab — Sky Shader
// Renders the sky sphere with gradient or equirectangular texture mapping.
// Renders at the far plane (depth = 1.0) with depth write disabled.

Shader "VibeEngine/Sky" {
    Properties {
        _MainTex ("Sky Texture", 2D) = "" {}
        _TopColor ("Top Color", Color) = (0.4, 0.7, 1.0, 1.0)
        _BottomColor ("Bottom Color", Color) = (0.9, 0.9, 0.95, 1.0)
    }

    SubShader {
        Tags { "RenderType"="Background" "Queue"="Background" }

        Pass {
            Name "SkyPass"
            Tags { "LightMode"="Always" }

            Cull Front
            ZWrite Off
            ZTest LEqual

            GLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag

#version 460 core

#ifdef VERTEX
layout(location = 0) in vec3 a_Position;

uniform mat4 u_MVP;

out vec3 v_Dir;

void main() {
    v_Dir = a_Position;
    vec4 pos = u_MVP * vec4(a_Position, 1.0);
    gl_Position = pos.xyww; // depth = 1.0 (far plane)
}
#endif

#ifdef FRAGMENT
in vec3 v_Dir;

uniform vec3 u_TopColor;
uniform vec3 u_BottomColor;
uniform sampler2D u_Texture;
uniform int u_UseTexture;

out vec4 FragColor;

void main() {
    vec3 dir = normalize(v_Dir);
    if (u_UseTexture == 1) {
        // Equirectangular mapping
        float u = atan(dir.z, dir.x) / (2.0 * 3.14159265) + 0.5;
        float v = asin(clamp(dir.y, -1.0, 1.0)) / 3.14159265 + 0.5;
        FragColor = vec4(texture(u_Texture, vec2(u, v)).rgb, 1.0);
    } else {
        float t = dir.y * 0.5 + 0.5; // 0 = bottom, 1 = top
        vec3 color = mix(u_BottomColor, u_TopColor, t);
        FragColor = vec4(color, 1.0);
    }
}
#endif

            ENDGLSL
        }
    }

    FallBack Off
}
