// VibeEngine ShaderLab — Decal Shader
// Projects a texture onto scene geometry using depth buffer reconstruction.
// Renders back faces of a cube volume so it works when camera is inside.
// Requires depth texture bound to unit 7.

Shader "VibeEngine/Decal" {
    Properties {
        _MainTex ("Decal Texture", 2D) = "white" {}
        _EntityColor ("Decal Color", Color) = (1, 1, 1, 1)
    }

    SubShader {
        Tags { "RenderType"="Transparent" "Queue"="Transparent" }

        Pass {
            Name "DecalPass"
            Tags { "LightMode"="Always" }

            Cull Front
            ZWrite Off
            ZTest LEqual
            Blend SrcAlpha OneMinusSrcAlpha

            GLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag

#version 460 core

#ifdef VERTEX
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec3 a_Color;
layout(location = 3) in vec2 a_TexCoord;

uniform mat4 u_MVP;
uniform mat4 u_Model;

out vec4 v_ClipPos;

void main() {
    gl_Position = u_MVP * vec4(a_Position, 1.0);
    v_ClipPos = gl_Position;
}
#endif

#ifdef FRAGMENT
in vec4 v_ClipPos;

uniform sampler2D u_DepthTexture;   // scene depth buffer (unit 7)
uniform sampler2D u_MainTex;        // decal texture
uniform int       u_HasMainTex;
uniform sampler2D u_Texture;        // legacy compat
uniform int       u_UseTexture;
uniform vec4      u_EntityColor;    // decal color tint

uniform mat4 u_InvVP;              // inverse view-projection
uniform mat4 u_InvModel;           // inverse model (world -> decal local)
uniform vec4 u_ScreenSize;         // viewport width, height (xy), unused (zw)

uniform float u_NormalBlend;       // 0-1
uniform float u_FadeDistance;      // soft edge fade

out vec4 FragColor;

void main() {
    // Screen-space UV from fragment position
    vec2 screenUV = gl_FragCoord.xy / u_ScreenSize.xy;

    // Sample scene depth
    float depth = texture(u_DepthTexture, screenUV).r;

    // Reconstruct world position from depth
    vec4 clipPos = vec4(screenUV * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 worldPos4 = u_InvVP * clipPos;
    vec3 worldPos = worldPos4.xyz / worldPos4.w;

    // Transform world position into decal's local space
    // Decal volume is a unit cube [-0.5, 0.5] in local space
    vec3 localPos = (u_InvModel * vec4(worldPos, 1.0)).xyz;

    // Check if inside the decal volume
    vec3 absLocal = abs(localPos);
    if (absLocal.x > 0.5 || absLocal.y > 0.5 || absLocal.z > 0.5)
        discard;

    // Compute UVs from local XZ position (project along Y axis)
    vec2 decalUV = localPos.xz + 0.5;

    // Sample decal texture
    vec4 decalColor = u_EntityColor;
    if (u_HasMainTex == 1)
        decalColor *= texture(u_MainTex, decalUV);
    else if (u_UseTexture == 1)
        decalColor *= texture(u_Texture, decalUV);

    // Soft edge fade — fade out near the boundaries of the volume
    float fadeX = smoothstep(0.5, 0.5 - u_FadeDistance, absLocal.x);
    float fadeY = smoothstep(0.5, 0.5 - u_FadeDistance, absLocal.y);
    float fadeZ = smoothstep(0.5, 0.5 - u_FadeDistance, absLocal.z);
    float fade = fadeX * fadeY * fadeZ;

    decalColor.a *= fade;

    // Discard fully transparent fragments
    if (decalColor.a < 0.001)
        discard;

    FragColor = decalColor;
}
#endif

            ENDGLSL
        }
    }

    FallBack Off
}
