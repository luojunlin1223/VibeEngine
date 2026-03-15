// VibeEngine ShaderLab — G-Buffer Pass for Deferred Rendering
// Writes geometry data to multiple render targets (MRT):
//   RT0: Position.xyz + Metallic (RGBA16F)
//   RT1: Normal.xyz   + Roughness (RGBA16F)
//   RT2: Albedo.rgb   + AO (RGBA8)
//   RT3: Emission.rgb + Flags (RGBA8)

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
        Tags { "RenderType"="Opaque" "Queue"="Geometry" "LightMode"="Deferred" }

        Pass {
            Name "GBufferPass"
            Tags { "LightMode"="Deferred" }

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

out vec3 v_Color;
out vec3 v_Normal;
out vec3 v_FragPos;
out vec2 v_TexCoord;

void main() {
    v_Color    = a_Color;
    v_TexCoord = a_TexCoord;
    v_Normal   = normalize(mat3(transpose(inverse(u_Model))) * a_Normal);
    v_FragPos  = vec3(u_Model * vec4(a_Position, 1.0));
    gl_Position = u_MVP * vec4(a_Position, 1.0);
}
#endif

#ifdef FRAGMENT
in vec3 v_Color;
in vec3 v_Normal;
in vec3 v_FragPos;
in vec2 v_TexCoord;

// Material properties
uniform vec4  u_EntityColor;
uniform float u_Metallic;
uniform float u_Roughness;
uniform float u_AO;
uniform float u_BumpScale;
uniform float u_OcclusionStrength;
uniform vec4  u_EmissionColor;
uniform float u_Cutoff;

// Material textures
uniform sampler2D u_MainTex;
uniform sampler2D u_MetallicGlossMap;
uniform sampler2D u_BumpMap;
uniform sampler2D u_OcclusionMap;
uniform sampler2D u_EmissionMap;

// Per-texture presence flags
uniform int u_HasMainTex;
uniform int u_HasMetallicGlossMap;
uniform int u_HasBumpMap;
uniform int u_HasOcclusionMap;
uniform int u_HasEmissionMap;

// Legacy compat
uniform sampler2D u_Texture;
uniform int   u_UseTexture;

// G-Buffer outputs (MRT)
layout(location = 0) out vec4 gPositionMetallic;  // RT0: pos.xyz + metallic
layout(location = 1) out vec4 gNormalRoughness;    // RT1: normal.xyz + roughness
layout(location = 2) out vec4 gAlbedoAO;           // RT2: albedo.rgb + ao
layout(location = 3) out vec4 gEmissionFlags;      // RT3: emission.rgb + flags

// ── Cotangent-frame Normal Mapping ──────────────────────────────────

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

vec3 perturbNormal(vec3 N, vec3 fragPos, vec2 uv) {
    vec3 mapN = texture(u_BumpMap, uv).xyz * 2.0 - 1.0;
    mapN.xy *= u_BumpScale;
    mapN = normalize(mapN);
    // Use fragPos direction as view (we only need TBN, not actual view dir)
    mat3 TBN = cotangentFrame(N, fragPos, uv);
    return normalize(TBN * mapN);
}

void main() {
    // ── Sample base color (albedo) ───────────────────────────────────
    vec4 baseColor = u_EntityColor;
    baseColor.rgb *= v_Color;

    if (u_HasMainTex == 1)
        baseColor *= texture(u_MainTex, v_TexCoord);
    else if (u_UseTexture == 1)
        baseColor *= texture(u_Texture, v_TexCoord);

    // ── Alpha clipping ───────────────────────────────────────────────
    if (u_Cutoff > 0.0 && baseColor.a < u_Cutoff)
        discard;

    // ── Metallic ─────────────────────────────────────────────────────
    float metallic = u_Metallic;
    if (u_HasMetallicGlossMap == 1) {
        vec4 metallicSample = texture(u_MetallicGlossMap, v_TexCoord);
        metallic *= metallicSample.r;
    }

    float roughness = max(u_Roughness, 0.04);

    // ── Occlusion ────────────────────────────────────────────────────
    float ao = u_AO;
    if (u_HasOcclusionMap == 1) {
        float occSample = texture(u_OcclusionMap, v_TexCoord).r;
        ao *= mix(1.0, occSample, u_OcclusionStrength);
    }

    // ── Normal ───────────────────────────────────────────────────────
    vec3 N = normalize(v_Normal);
    if (u_HasBumpMap == 1)
        N = perturbNormal(N, v_FragPos, v_TexCoord);

    // ── Emission ─────────────────────────────────────────────────────
    vec3 emission = u_EmissionColor.rgb;
    if (u_HasEmissionMap == 1)
        emission *= texture(u_EmissionMap, v_TexCoord).rgb;

    // ── Write G-Buffer ───────────────────────────────────────────────
    gPositionMetallic = vec4(v_FragPos, metallic);
    gNormalRoughness  = vec4(N * 0.5 + 0.5, roughness); // encode normal to [0,1]
    gAlbedoAO         = vec4(baseColor.rgb, ao);
    gEmissionFlags    = vec4(emission, 0.0); // flags=0 for standard lit
}
#endif

            ENDGLSL
        }
    }

    FallBack "VibeEngine/Unlit"
}
