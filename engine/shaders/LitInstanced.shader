// VibeEngine ShaderLab — PBR Lit Instanced Shader
// GPU-instanced variant of Lit. Per-instance model matrix and color
// are provided via vertex attributes instead of uniforms.

Shader "VibeEngine/LitInstanced" {
    Properties {
        _MainTex ("Base Map", 2D) = "white" {}
        _Metallic ("Metallic", Range(0, 1)) = 0.0
        _Roughness ("Roughness", Range(0, 1)) = 0.5
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
        Tags { "RenderType"="Opaque" "Queue"="Geometry" "LightMode"="Forward" }

        Pass {
            Name "ForwardLitInstanced"
            Tags { "LightMode"="Forward" }

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
out vec3 v_Normal;
out vec3 v_FragPos;
out vec2 v_TexCoord;
out vec4 v_InstanceColor;

void main() {
    mat4 model  = a_InstanceModel;
    v_Color     = a_Color;
    v_TexCoord  = a_TexCoord;
    v_Normal    = normalize(mat3(transpose(inverse(model))) * a_Normal);
    v_FragPos   = vec3(model * vec4(a_Position, 1.0));
    v_InstanceColor = a_InstanceColor;
    gl_Position = u_ViewProjection * model * vec4(a_Position, 1.0);
}
#endif

#ifdef FRAGMENT
in vec3 v_Color;
in vec3 v_Normal;
in vec3 v_FragPos;
in vec2 v_TexCoord;
in vec4 v_InstanceColor;

// Material properties
uniform float u_Metallic;
uniform float u_Roughness;
uniform float u_AO;
uniform float u_BumpScale;
uniform float u_OcclusionStrength;
uniform vec4  u_EmissionColor;
uniform float u_Cutoff;

// Material textures
uniform sampler2D u_MainTex;
uniform sampler2D u_BumpMap;
uniform sampler2D u_OcclusionMap;
uniform sampler2D u_EmissionMap;

uniform int u_HasMainTex;
uniform int u_HasBumpMap;
uniform int u_HasOcclusionMap;
uniform int u_HasEmissionMap;

// Legacy compat
uniform sampler2D u_Texture;
uniform int   u_UseTexture;

// Lighting, shadows, PBR, normal mapping — shared includes
#include "lighting.glslinc"
#include "shadows.glslinc"
#include "common.glslinc"
#include "brdf.glslinc"
#include "normal_mapping.glslinc"

// Reflection probe uniforms
uniform int          u_HasReflectionProbe;
uniform samplerCube  u_ReflectionProbe;
uniform float        u_ReflectionIntensity;

out vec4 FragColor;

void main() {
    vec4 baseColor = v_InstanceColor;
    baseColor.rgb *= v_Color;

    if (u_HasMainTex == 1)
        baseColor *= texture(u_MainTex, v_TexCoord);
    else if (u_UseTexture == 1)
        baseColor *= texture(u_Texture, v_TexCoord);

    if (u_Cutoff > 0.0 && baseColor.a < u_Cutoff)
        discard;

    vec3 albedo = baseColor.rgb;

    float metallic = u_Metallic;
    float roughness = max(u_Roughness, 0.04);

    float ao = u_AO;
    if (u_HasOcclusionMap == 1) {
        float occSample = texture(u_OcclusionMap, v_TexCoord).r;
        ao *= mix(1.0, occSample, u_OcclusionStrength);
    }

    vec3 N = normalize(v_Normal);
    vec3 V = normalize(u_ViewPos - v_FragPos);

    if (u_HasBumpMap == 1)
        N = perturbNormal(N, V, v_TexCoord);

    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 Lo = vec3(0.0);

    // Directional light
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
        float shadow = ShadowCalculation(v_FragPos, N, L);
        Lo += (kD * albedo / PI + spec) * radiance * NdotL * shadow;
    }

    // Point lights
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
        float pointShadow = PointShadowCalculation(u_PointLightShadowIndex[i], v_FragPos, u_PointLightPositions[i]);
        Lo += (kD * albedo / PI + spec) * radiance * NdotL * pointShadow;
    }

    // Spot lights
    for (int i = 0; i < u_NumSpotLights; ++i) {
        vec3  lightVec  = u_SpotLightPositions[i] - v_FragPos;
        float dist      = length(lightVec);
        float range     = u_SpotLightRanges[i];
        if (dist > range) continue;
        vec3  L = lightVec / dist;
        vec3  H = normalize(V + L);
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
        float spotShadow = SpotShadowCalculation(u_SpotLightShadowIndex[i], v_FragPos, N);
        Lo += (kD * albedo / PI + spec) * radiance * NdotL * spotShadow;
    }

    // Spot lights
    for (int i = 0; i < u_NumSpotLights; ++i) {
        vec3  lightVec  = u_SpotLightPositions[i] - v_FragPos;
        float dist      = length(lightVec);
        float range     = u_SpotLightRanges[i];
        if (dist > range) continue;
        vec3  L = lightVec / dist;
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
        vec3 R = reflect(-V, N);
        float maxMipLevel = 4.0;
        float mipLevel = roughness * maxMipLevel;
        vec3 envColor = textureLod(u_ReflectionProbe, R, mipLevel).rgb;
        float NdotV2 = max(dot(N, V), 0.0);
        vec3 F = FresnelSchlickRoughness(NdotV2, F0, roughness);
        vec3 kS = F;
        vec3 kD = (1.0 - kS) * (1.0 - metallic);
        vec3 irradiance = textureLod(u_ReflectionProbe, N, maxMipLevel).rgb;
        vec3 diffuseIBL = irradiance * albedo;
        vec3 specularIBL = envColor * F;
        ambient = (kD * diffuseIBL + specularIBL) * ao * u_ReflectionIntensity;
    } else {
        ambient = vec3(0.03) * albedo * ao;
    }
    vec3 color = ambient + Lo;

    vec3 emission = u_EmissionColor.rgb;
    if (u_HasEmissionMap == 1)
        emission *= texture(u_EmissionMap, v_TexCoord).rgb;
    color += emission;

    // Output linear HDR — tone mapping and gamma handled by post-processing
    FragColor = vec4(color, baseColor.a);
}
#endif

            ENDGLSL
        }
    }

    FallBack "VibeEngine/Unlit"
}
