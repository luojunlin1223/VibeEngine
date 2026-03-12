// VibeEngine ShaderLab — Unlit Shader
// Used for 2D meshes (Triangle, Quad) without lighting calculations.

Shader "VibeEngine/Unlit" {
    Properties {
        _MainTex ("Main Texture", 2D) = "white" {}
        _Color ("Color", Color) = (1, 1, 1, 1)
    }

    SubShader {
        Tags { "RenderType"="Opaque" "Queue"="Geometry" }

        Pass {
            Name "Default"
            Tags { "LightMode"="Always" }

            Cull Back
            ZWrite On
            ZTest LEqual

            GLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag

#version 460 core

#ifdef VERTEX
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Color;
layout(location = 2) in vec2 a_TexCoord;

uniform mat4 u_MVP;

out vec3 v_Color;
out vec2 v_TexCoord;

void main() {
    v_Color    = a_Color;
    v_TexCoord = a_TexCoord;
    gl_Position = u_MVP * vec4(a_Position, 1.0);
}
#endif

#ifdef FRAGMENT
in vec3 v_Color;
in vec2 v_TexCoord;

uniform sampler2D u_Texture;
uniform int u_UseTexture;
uniform vec4 u_EntityColor;

out vec4 FragColor;

void main() {
    vec3 baseColor = v_Color;
    if (u_UseTexture == 1)
        baseColor = texture(u_Texture, v_TexCoord).rgb;
    baseColor *= u_EntityColor.rgb;
    FragColor = vec4(baseColor, u_EntityColor.a);
}
#endif

            ENDGLSL
        }
    }

    FallBack Off
}
