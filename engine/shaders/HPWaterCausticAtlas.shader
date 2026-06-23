// VibeEngine ShaderLab - HPWater light-space caustic atlas capture
// Writes per-cascade water surface payloads for the HPWater caustic pipeline.

Shader "VibeEngine/HPWaterCausticAtlas" {
    Properties {
    }

    SubShader {
        Tags { "RenderType"="Opaque" "Queue"="Geometry" "LightMode"="HPWaterCausticAtlas" }

        Pass {
            Name "HPWaterCausticAtlasPass"

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

uniform mat4 u_LightMVP;
uniform mat4 u_Model;

out vec3 v_Normal;
out vec3 v_FragPos;
out vec2 v_TexCoord;

void main() {
    v_Normal = normalize(mat3(u_Model) * a_Normal);
    v_FragPos = vec3(u_Model * vec4(a_Position, 1.0));
    v_TexCoord = a_TexCoord;
    gl_Position = u_LightMVP * vec4(a_Position, 1.0);
}
#endif

#ifdef FRAGMENT
in vec3 v_Normal;
in vec3 v_FragPos;
in vec2 v_TexCoord;

uniform float u_HPRoughness;
uniform float u_HPThickness;

layout(location = 0) out vec4 gCausticAtlas;

void main() {
    vec3 N = normalize(v_Normal);
    float thickness = clamp(u_HPThickness / 64.0, 0.0, 1.0);
    float roughness = clamp(u_HPRoughness, 0.015, 0.75);
    gCausticAtlas = vec4(N * 0.5 + 0.5, max(thickness, roughness * 0.25));
}
#endif

            ENDGLSL
        }
    }

    FallBack "VibeEngine/HPWaterGBuffer"
}
