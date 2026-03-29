// VibeEngine ShaderLab — Deferred Lighting Pass
// Fullscreen quad that reads G-buffer textures and computes PBR lighting.
// Uses the same Cook-Torrance BRDF as the forward Lit.shader.

Shader "VibeEngine/DeferredLighting" {
    Properties {
    }

    SubShader {
        Tags { "RenderType"="Opaque" "Queue"="Overlay" "LightMode"="DeferredLighting" }

        Pass {
            Name "DeferredLightingPass"

            Cull Off
            ZWrite Off
            ZTest Always

            GLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag

#version 460 core

#ifdef VERTEX
layout(location = 0) out vec2 v_UV;
void main() {
    // Fullscreen triangle from gl_VertexID (no VBO needed)
    vec2 pos = vec2((gl_VertexID & 1) * 2.0, (gl_VertexID & 2) * 1.0);
    v_UV = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
#endif

#ifdef FRAGMENT
layout(location = 0) in vec2 v_UV;
layout(location = 0) out vec4 FragColor;

// G-Buffer samplers
uniform sampler2D u_GPositionMetallic;  // RT0
uniform sampler2D u_GNormalRoughness;   // RT1
uniform sampler2D u_GAlbedoAO;          // RT2
uniform sampler2D u_GEmissionFlags;     // RT3

// Lighting, shadows, PBR — shared includes
#include "lighting.glslinc"
#include "shadows.glslinc"
#include "common.glslinc"
#include "brdf.glslinc"

// ── Compute PBR lighting for one light ───────────────────────────────

vec3 ComputePBR(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 albedo,
                float metallic, float roughness, vec3 F0) {
    vec3 H = normalize(V + L);

    float NDF = DistributionGGX(N, H, roughness);
    float G   = GeometrySmith(N, V, L, roughness);
    vec3  F   = FresnelSchlick(max(dot(H, V), 0.0), F0);

    vec3  spec = (NDF * G * F) / max(4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0), 0.001);
    vec3  kD = (vec3(1.0) - F) * (1.0 - metallic);

    float NdotL = max(dot(N, L), 0.0);

    return (kD * albedo / PI + spec) * radiance * NdotL;
}

// ── Main ─────────────────────────────────────────────────────────────

void main() {
    // Sample G-Buffer
    vec4 posMetallic    = texture(u_GPositionMetallic, v_UV);
    vec4 normRoughness  = texture(u_GNormalRoughness,  v_UV);
    vec4 albedoAO       = texture(u_GAlbedoAO,         v_UV);
    vec4 emissionFlags  = texture(u_GEmissionFlags,    v_UV);

    vec3  fragPos   = posMetallic.xyz;
    float metallic  = posMetallic.w;
    vec3  N         = normalize(normRoughness.xyz * 2.0 - 1.0); // decode from [0,1]
    float roughness = normRoughness.w;
    vec3  albedo    = albedoAO.rgb;
    float ao        = albedoAO.a;
    vec3  emission  = emissionFlags.rgb;

    // Discard sky pixels (position at origin with zero metallic can be checked,
    // but more reliable: check if albedo+normal are both zero)
    if (length(normRoughness.xyz) < 0.01)
        discard;

    vec3 V = normalize(u_ViewPos - fragPos);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    vec3 Lo = vec3(0.0);

    // ── Directional light ────────────────────────────────────────────
    {
        vec3 L = normalize(u_LightDir);
        vec3 radiance = u_LightColor * u_LightIntensity;
        float shadow = ShadowCalculation(fragPos, N, L);
        Lo += ComputePBR(N, V, L, radiance, albedo, metallic, roughness, F0) * shadow;
    }

    // ── Point lights ─────────────────────────────────────────────────
    for (int i = 0; i < u_NumPointLights; ++i) {
        vec3  lightVec  = u_PointLightPositions[i] - fragPos;
        float dist      = length(lightVec);
        float range     = u_PointLightRanges[i];

        if (dist > range) continue;

        vec3  L = lightVec / dist;

        float attenuation = 1.0 / (dist * dist + 1.0);
        float window = 1.0 - pow(clamp(dist / range, 0.0, 1.0), 4.0);
        window = window * window;
        attenuation *= window;

        vec3 radiance = u_PointLightColors[i] * u_PointLightIntensities[i] * attenuation;
        float pointShadow = PointShadowCalculation(u_PointLightShadowIndex[i], fragPos, u_PointLightPositions[i]);
        Lo += ComputePBR(N, V, L, radiance, albedo, metallic, roughness, F0) * pointShadow;
    }

    // ── Spot lights ─────────────────────────────────────────────────
    for (int i = 0; i < u_NumSpotLights; ++i) {
        vec3  lightVec  = u_SpotLightPositions[i] - fragPos;
        float dist      = length(lightVec);
        float range     = u_SpotLightRanges[i];

        if (dist > range) continue;

        vec3  L = lightVec / dist;

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
        float spotShadow = SpotShadowCalculation(u_SpotLightShadowIndex[i], fragPos, N);
        Lo += ComputePBR(N, V, L, radiance, albedo, metallic, roughness, F0) * spotShadow;
    }

    // ── Ambient + Occlusion ──────────────────────────────────────────
    vec3 ambient = vec3(0.03) * albedo * ao;
    vec3 color = ambient + Lo + emission;

    // Output linear HDR — tone mapping and gamma handled by post-processing
    FragColor = vec4(color, 1.0);
}
#endif

            ENDGLSL
        }
    }

    FallBack "VibeEngine/Unlit"
}
