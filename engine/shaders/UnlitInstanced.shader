// VibeEngine ShaderLab — Unlit Instanced Shader
// GPU-instanced variant of Unlit. Per-instance model matrix and color
// are provided via vertex attributes instead of uniforms.

Shader "VibeEngine/UnlitInstanced" {
    Properties {
        _MainTex ("Main Texture", 2D) = "white" {}
    }

    SubShader {
        Tags { "RenderType"="Opaque" "Queue"="Geometry" }

        Pass {
            Name "DefaultInstanced"
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
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec3 a_Color;
layout(location = 3) in vec2 a_TexCoord;

// Per-instance data (mat4 uses locations 4-7, color at 8)
layout(location = 4) in mat4 a_InstanceModel;
layout(location = 8) in vec4 a_InstanceColor;

uniform mat4 u_ViewProjection;

out vec3 v_Color;
out vec2 v_TexCoord;
out vec4 v_InstanceColor;

void main() {
    v_Color         = a_Color;
    v_TexCoord      = a_TexCoord;
    v_InstanceColor = a_InstanceColor;
    gl_Position     = u_ViewProjection * a_InstanceModel * vec4(a_Position, 1.0);
}
#endif

#ifdef FRAGMENT
in vec3 v_Color;
in vec2 v_TexCoord;
in vec4 v_InstanceColor;

uniform sampler2D u_Texture;
uniform int u_UseTexture;

out vec4 FragColor;

void main() {
    vec3 baseColor = v_Color;
    if (u_UseTexture == 1)
        baseColor = texture(u_Texture, v_TexCoord).rgb;
    baseColor *= v_InstanceColor.rgb;
    FragColor = vec4(baseColor, v_InstanceColor.a);
}
#endif

            ENDGLSL
        }
    }

    FallBack Off
}
