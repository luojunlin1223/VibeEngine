// VibeEngine ShaderLab -- G-Buffer pass for Deferred+ pipeline
// Writes geometry data to 4 MRT: Position+Metallic, Normal+Roughness, Albedo+AO, Emission+Flags

Shader "VibeEngine/GBuffer" {
    Properties {
        _MainTex ("Base Map", 2D) = "white" {}
        _EntityColor ("Color", Color) = (1, 1, 1, 1)
        _Metallic ("Metallic", Range(0, 1)) = 0.0
        _Roughness ("Roughness", Range(0, 1)) = 0.5
        _MetallicGlossMap ("Metallic Map", 2D) = "white" {}
        _BumpMap ("Normal Map", 2D) = "bump" {}
        _BumpScale ("Normal Scale", Range(0, 2)) = 1.0
        _OcclusionMap ("Occlusion Map", 2D) = "white" {}
        _OcclusionStrength ("Occlusion Strength", Range(0, 1)) = 1.0
        _EmissionMap ("Emission Map", 2D) = "black" {}
        _EmissionColor ("Emission Color", Color) = (0, 0, 0, 1)
        _Cutoff ("Alpha Cutoff", Range(0, 1)) = 0.0
        _AO ("Ambient Occlusion", Range(0, 1)) = 1.0
    }

    SubShader {
        Tags { "RenderType"="Opaque" "Queue"="Geometry" "LightMode"="GBuffer" }

        Pass {
            Name "GBuffer"
            Tags { "LightMode"="GBuffer" }

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

uniform mat4 u_MVP;
uniform mat4 u_Model;

out vec3 v_FragPos;
out vec3 v_Normal;
out vec3 v_Color;
out vec2 v_TexCoord;

void main() {
    vec4 worldPos = u_Model * vec4(a_Position, 1.0);
    v_FragPos  = worldPos.xyz;
    v_Normal   = normalize(mat3(transpose(inverse(u_Model))) * a_Normal);
    v_Color    = a_Color;
    v_TexCoord = a_TexCoord;
    gl_Position = u_MVP * vec4(a_Position, 1.0);
}
#endif

#ifdef FRAGMENT
in vec3 v_FragPos;
in vec3 v_Normal;
in vec3 v_Color;
in vec2 v_TexCoord;

layout(location = 0) out vec4 gPositionMetallic;
layout(location = 1) out vec4 gNormalRoughness;
layout(location = 2) out vec4 gAlbedoAO;
layout(location = 3) out vec4 gEmissionFlags;

uniform vec4  u_EntityColor;
uniform float u_Metallic;
uniform float u_Roughness;
uniform float u_AO;
uniform float u_BumpScale;
uniform float u_OcclusionStrength;
uniform vec4  u_EmissionColor;
uniform float u_Cutoff;

uniform sampler2D u_MainTex;
uniform sampler2D u_MetallicGlossMap;
uniform sampler2D u_BumpMap;
uniform sampler2D u_OcclusionMap;
uniform sampler2D u_EmissionMap;

uniform int u_HasMainTex;
uniform int u_HasMetallicGlossMap;
uniform int u_HasBumpMap;
uniform int u_HasOcclusionMap;
uniform int u_HasEmissionMap;

uniform sampler2D u_Texture;
uniform int u_UseTexture;

// Cotangent-frame normal mapping
mat3 cotangentFrame(vec3 N, vec3 p, vec2 uv) {
    vec3 dp1 = dFdx(p);
    vec3 dp2 = dFdy(p);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, N);
    vec3 dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = inversesqrt(max(dot(T, T), dot(B, B)));
    return mat3(T * invmax, B * invmax, N);
}

vec3 perturbNormal(vec3 N, vec3 V, vec2 uv) {
    vec3 mapN = texture(u_BumpMap, uv).xyz * 2.0 - 1.0;
    mapN.xy *= u_BumpScale;
    mapN = normalize(mapN);
    mat3 TBN = cotangentFrame(N, -V, uv);
    return normalize(TBN * mapN);
}

// We need u_ViewPos for normal mapping direction
uniform vec3 u_ViewPos;

void main() {
    // Albedo
    vec4 baseColor = u_EntityColor;
    baseColor.rgb *= v_Color;
    if (u_HasMainTex == 1)
        baseColor *= texture(u_MainTex, v_TexCoord);
    else if (u_UseTexture == 1)
        baseColor *= texture(u_Texture, v_TexCoord);

    if (u_Cutoff > 0.0 && baseColor.a < u_Cutoff)
        discard;

    // Metallic
    float metallic = u_Metallic;
    if (u_HasMetallicGlossMap == 1)
        metallic *= texture(u_MetallicGlossMap, v_TexCoord).r;

    float roughness = max(u_Roughness, 0.04);

    // AO
    float ao = u_AO;
    if (u_HasOcclusionMap == 1) {
        float occSample = texture(u_OcclusionMap, v_TexCoord).r;
        ao *= mix(1.0, occSample, u_OcclusionStrength);
    }

    // Normal
    vec3 N = normalize(v_Normal);
    if (u_HasBumpMap == 1) {
        vec3 V = normalize(u_ViewPos - v_FragPos);
        N = perturbNormal(N, V, v_TexCoord);
    }

    // Emission
    vec3 emission = u_EmissionColor.rgb;
    if (u_HasEmissionMap == 1)
        emission *= texture(u_EmissionMap, v_TexCoord).rgb;

    // Write G-Buffer
    gPositionMetallic = vec4(v_FragPos, metallic);
    gNormalRoughness  = vec4(N, roughness);
    gAlbedoAO         = vec4(baseColor.rgb, ao);
    gEmissionFlags    = vec4(emission, 1.0);
}
#endif

            ENDGLSL
        }
    }

    FallBack "VibeEngine/Unlit"
}
