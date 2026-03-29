// VibeEngine ShaderLab — PBR Lit Shader
// Physically-Based Rendering with Cook-Torrance BRDF, inspired by Unity URP Lit.
// Supports: Base Map, Normal Map, Metallic Map, Occlusion Map, Emission,
//           Alpha Clipping, up to 8 Point Lights, up to 4 Spot Lights.

Shader "VibeEngine/Lit" {
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
        Tags { "RenderType"="Opaque" "Queue"="Geometry" "LightMode"="Lit" }

        Pass {
            Name "ForwardLit"
            Tags { "LightMode"="Lit" }

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

// Per-texture presence flags (set by Material::Bind)
uniform int u_HasMainTex;
uniform int u_HasMetallicGlossMap;
uniform int u_HasBumpMap;
uniform int u_HasOcclusionMap;
uniform int u_HasEmissionMap;

// Legacy compat
uniform sampler2D u_Texture;
uniform int   u_UseTexture;

// Lighting, PBR, normal mapping — shared includes
#include "lighting.glslinc"
#include "common.glslinc"
#include "brdf.glslinc"
#include "normal_mapping.glslinc"

// Reflection probe uniforms
uniform int          u_HasReflectionProbe;
uniform samplerCube  u_ReflectionProbe;
uniform float        u_ReflectionIntensity;

out vec4 FragColor;

// ── Main ─────────────────────────────────────────────────────────────

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

    vec3 albedo = baseColor.rgb;

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
    vec3 V = normalize(u_ViewPos - v_FragPos);

    if (u_HasBumpMap == 1)
        N = perturbNormal(N, V, v_TexCoord);

    // ── PBR setup ────────────────────────────────────────────────────
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    vec3 Lo = vec3(0.0);

    // ── Directional light ────────────────────────────────────────────
    {
        vec3 L = normalize(u_LightDir);
        vec3 H = normalize(V + L);
        vec3 radiance = u_LightColor * u_LightIntensity;

        float NDF = DistributionGGX(N, H, roughness);
        float G   = GeometrySmith(N, V, L, roughness);
        vec3  F   = FresnelSchlick(max(dot(H, V), 0.0), F0);

        vec3  spec = (NDF * G * F) / max(4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0), 0.001);
        vec3  kD = (vec3(1.0) - F) * (1.0 - metallic);

        float NdotL = max(dot(N, L), 0.0);

        Lo += (kD * albedo / PI + spec) * radiance * NdotL;
    }

    // ── Point lights ─────────────────────────────────────────────────
    for (int i = 0; i < u_NumPointLights; ++i) {
        vec3  lightVec  = u_PointLightPositions[i] - v_FragPos;
        float dist      = length(lightVec);
        float range     = u_PointLightRanges[i];

        if (dist > range) continue;

        vec3  L = lightVec / dist;
        vec3  H = normalize(V + L);

        float attenuation = 1.0 / (dist * dist + 1.0);
        float window = 1.0 - pow(clamp(dist / range, 0.0, 1.0), 4.0);
        window = window * window;
        attenuation *= window;

        vec3 radiance = u_PointLightColors[i] * u_PointLightIntensities[i] * attenuation;

        float NDF = DistributionGGX(N, H, roughness);
        float G   = GeometrySmith(N, V, L, roughness);
        vec3  F   = FresnelSchlick(max(dot(H, V), 0.0), F0);

        vec3  spec = (NDF * G * F) / max(4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0), 0.001);
        vec3  kD = (vec3(1.0) - F) * (1.0 - metallic);

        float NdotL = max(dot(N, L), 0.0);

        Lo += (kD * albedo / PI + spec) * radiance * NdotL;
    }

    // ── Spot lights ─────────────────────────────────────────────────
    for (int i = 0; i < u_NumSpotLights; ++i) {
        vec3  lightVec  = u_SpotLightPositions[i] - v_FragPos;
        float dist      = length(lightVec);
        float range     = u_SpotLightRanges[i];

        if (dist > range) continue;

        vec3  L = lightVec / dist;
        vec3  H = normalize(V + L);

        // Spot cone attenuation
        float theta = dot(L, normalize(-u_SpotLightDirections[i]));
        float epsilon = u_SpotLightInnerCos[i] - u_SpotLightOuterCos[i];
        float spotFactor = clamp((theta - u_SpotLightOuterCos[i]) / max(epsilon, 0.0001), 0.0, 1.0);

        if (spotFactor <= 0.0) continue;

        float attenuation = 1.0 / (dist * dist + 1.0);
        float window = 1.0 - pow(clamp(dist / range, 0.0, 1.0), 4.0);
        window = window * window;
        attenuation *= window * spotFactor;

        vec3 radiance = u_SpotLightColors[i] * u_SpotLightIntensities[i] * attenuation;

        float NDF = DistributionGGX(N, H, roughness);
        float G   = GeometrySmith(N, V, L, roughness);
        vec3  F   = FresnelSchlick(max(dot(H, V), 0.0), F0);

        vec3  spec = (NDF * G * F) / max(4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0), 0.001);
        vec3  kD = (vec3(1.0) - F) * (1.0 - metallic);

        float NdotL = max(dot(N, L), 0.0);

        Lo += (kD * albedo / PI + spec) * radiance * NdotL;
    }

    // ── Spot lights ──────────────────────────────────────────────────
    for (int i = 0; i < u_NumSpotLights; ++i) {
        vec3  lightVec  = u_SpotLightPositions[i] - v_FragPos;
        float dist      = length(lightVec);
        float range     = u_SpotLightRanges[i];

        if (dist > range) continue;

        vec3  L = lightVec / dist;

        // Spot cone attenuation
        float theta   = dot(L, normalize(-u_SpotLightDirections[i]));
        float epsilon = u_SpotLightInnerCos[i] - u_SpotLightOuterCos[i];
        float spotAtt = clamp((theta - u_SpotLightOuterCos[i]) / max(epsilon, 0.001), 0.0, 1.0);

        if (spotAtt <= 0.0) continue;

        vec3  H = normalize(V + L);

        float attenuation = 1.0 / (dist * dist + 1.0);
        float window = 1.0 - pow(clamp(dist / range, 0.0, 1.0), 4.0);
        window = window * window;
        attenuation *= window * spotAtt;

        vec3 radiance = u_SpotLightColors[i] * u_SpotLightIntensities[i] * attenuation;

        float NDF = DistributionGGX(N, H, roughness);
        float G   = GeometrySmith(N, V, L, roughness);
        vec3  F   = FresnelSchlick(max(dot(H, V), 0.0), F0);

        vec3  spec = (NDF * G * F) / max(4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0), 0.001);
        vec3  kD = (vec3(1.0) - F) * (1.0 - metallic);

        float NdotL = max(dot(N, L), 0.0);

        Lo += (kD * albedo / PI + spec) * radiance * NdotL;
    }

    // ── Ambient + IBL Reflections + Occlusion ──────────────────────────
    vec3 ambient;
    if (u_HasReflectionProbe == 1) {
        // Image-based lighting from reflection probe cubemap
        vec3 R = reflect(-V, N);

        // Sample cubemap with roughness-based mip level
        float maxMipLevel = 4.0; // log2(resolution/16) approx for 128 res
        float mipLevel = roughness * maxMipLevel;
        vec3 envColor = textureLod(u_ReflectionProbe, R, mipLevel).rgb;

        // Fresnel with roughness for environment reflections
        float NdotV = max(dot(N, V), 0.0);
        vec3 F = FresnelSchlickRoughness(NdotV, F0, roughness);

        // Split-sum approximation (simplified without a BRDF LUT):
        // kS = Fresnel, kD = 1 - kS (energy conservation)
        vec3 kS = F;
        vec3 kD = (1.0 - kS) * (1.0 - metallic);

        // Diffuse IBL: sample cubemap at normal direction (rough mip)
        vec3 irradiance = textureLod(u_ReflectionProbe, N, maxMipLevel).rgb;
        vec3 diffuseIBL = irradiance * albedo;

        // Specular IBL: environment reflection
        vec3 specularIBL = envColor * F;

        ambient = (kD * diffuseIBL + specularIBL) * ao * u_ReflectionIntensity;
    } else {
        ambient = vec3(0.03) * albedo * ao;
    }
    vec3 color = ambient + Lo;

    // ── Emission ─────────────────────────────────────────────────────
    vec3 emission = u_EmissionColor.rgb;
    if (u_HasEmissionMap == 1)
        emission *= texture(u_EmissionMap, v_TexCoord).rgb;
    color += emission;

    // Output linear HDR
    FragColor = vec4(color, baseColor.a);
}
#endif

            ENDGLSL
        }
    }

    FallBack "VibeEngine/Unlit"
}
