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

// Camera
uniform vec3 u_ViewPos;

// Directional light
uniform vec3  u_LightDir;
uniform vec3  u_LightColor;
uniform float u_LightIntensity;

// Point lights (max 8)
const int MAX_POINT_LIGHTS = 8;
uniform int   u_NumPointLights;
uniform vec3  u_PointLightPositions[MAX_POINT_LIGHTS];
uniform vec3  u_PointLightColors[MAX_POINT_LIGHTS];
uniform float u_PointLightIntensities[MAX_POINT_LIGHTS];
uniform float u_PointLightRanges[MAX_POINT_LIGHTS];
uniform int   u_PointLightShadowIndex[MAX_POINT_LIGHTS];

// Spot lights (max 4)
const int MAX_SPOT_LIGHTS = 4;
uniform int   u_NumSpotLights;
uniform vec3  u_SpotLightPositions[MAX_SPOT_LIGHTS];
uniform vec3  u_SpotLightDirections[MAX_SPOT_LIGHTS];
uniform vec3  u_SpotLightColors[MAX_SPOT_LIGHTS];
uniform float u_SpotLightIntensities[MAX_SPOT_LIGHTS];
uniform float u_SpotLightRanges[MAX_SPOT_LIGHTS];
uniform float u_SpotLightInnerCos[MAX_SPOT_LIGHTS];
uniform float u_SpotLightOuterCos[MAX_SPOT_LIGHTS];
uniform int   u_SpotLightShadowIndex[MAX_SPOT_LIGHTS];

// Shadow uniforms (directional CSM)
uniform int   u_ShadowEnabled;
uniform sampler2DArrayShadow u_ShadowMap;
uniform mat4  u_LightSpaceMatrices[3];
uniform vec3  u_CascadeSplits;
uniform mat4  u_ViewMatrix;
uniform float u_ShadowBias;
uniform float u_ShadowNormalBias;
uniform int   u_PCFRadius;

// Spot light shadow maps (max 2)
const int MAX_SPOT_SHADOWS = 2;
uniform int   u_NumSpotShadows;
uniform sampler2DShadow u_SpotShadowMaps[MAX_SPOT_SHADOWS];
uniform mat4  u_SpotLightSpaceMatrices[MAX_SPOT_SHADOWS];

// Point light shadow maps (max 2)
const int MAX_POINT_SHADOWS = 2;
uniform int   u_NumPointShadows;
uniform samplerCube u_PointShadowCubeMaps[MAX_POINT_SHADOWS];
uniform float u_PointShadowFarPlanes[MAX_POINT_SHADOWS];

const float PI = 3.14159265359;

// ── PBR Functions (identical to Lit.shader) ──────────────────────────

float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float NdotH  = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 0.0000001);
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    return GeometrySchlickGGX(max(dot(N, V), 0.0), roughness)
         * GeometrySchlickGGX(max(dot(N, L), 0.0), roughness);
}

vec3 FresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// ── Shadow Functions ─────────────────────────────────────────────────

float ShadowCalculation(vec3 fragPos, vec3 normal, vec3 lightDir) {
    if (u_ShadowEnabled == 0)
        return 1.0;

    vec4 fragPosViewSpace = u_ViewMatrix * vec4(fragPos, 1.0);
    float depthValue = -fragPosViewSpace.z;

    int cascade = 2;
    if (depthValue < u_CascadeSplits.x)
        cascade = 0;
    else if (depthValue < u_CascadeSplits.y)
        cascade = 1;

    vec3 biasedPos = fragPos + normal * u_ShadowNormalBias * (1.0 + float(cascade));

    vec4 fragPosLightSpace = u_LightSpaceMatrices[cascade] * vec4(biasedPos, 1.0);
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;

    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0 ||
        projCoords.z > 1.0)
        return 1.0;

    float cosTheta = max(dot(normal, lightDir), 0.0);
    float bias = u_ShadowBias * (1.0 - cosTheta);
    float currentDepth = projCoords.z - bias;

    float shadow = 0.0;
    vec2 texelSize = 1.0 / vec2(textureSize(u_ShadowMap, 0).xy);

    int samples = 0;
    for (int x = -u_PCFRadius; x <= u_PCFRadius; ++x) {
        for (int y = -u_PCFRadius; y <= u_PCFRadius; ++y) {
            vec2 offset = vec2(float(x), float(y)) * texelSize;
            shadow += texture(u_ShadowMap, vec4(projCoords.xy + offset, float(cascade), currentDepth));
            samples++;
        }
    }
    shadow /= float(samples);

    return shadow;
}

float SpotShadowCalculation(int shadowIdx, vec3 fragPos, vec3 normal) {
    if (shadowIdx < 0 || shadowIdx >= u_NumSpotShadows)
        return 1.0;

    vec4 fragPosLightSpace = u_SpotLightSpaceMatrices[shadowIdx] * vec4(fragPos, 1.0);
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;

    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0 ||
        projCoords.z > 1.0)
        return 1.0;

    float bias = 0.001;
    float currentDepth = projCoords.z - bias;

    float shadow = 0.0;
    vec2 texelSize;
    if (shadowIdx == 0)
        texelSize = 1.0 / vec2(textureSize(u_SpotShadowMaps[0], 0));
    else
        texelSize = 1.0 / vec2(textureSize(u_SpotShadowMaps[1], 0));

    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            vec3 coord = vec3(projCoords.xy + vec2(float(x), float(y)) * texelSize, currentDepth);
            if (shadowIdx == 0)
                shadow += texture(u_SpotShadowMaps[0], coord);
            else
                shadow += texture(u_SpotShadowMaps[1], coord);
        }
    }
    shadow /= 9.0;
    return shadow;
}

float PointShadowCalculation(int shadowIdx, vec3 fragPos, vec3 lightPos) {
    if (shadowIdx < 0 || shadowIdx >= u_NumPointShadows)
        return 1.0;

    vec3 fragToLight = fragPos - lightPos;
    float currentDist = length(fragToLight);
    float farPlane = u_PointShadowFarPlanes[shadowIdx];

    float bias = 0.05;
    float shadow = 0.0;

    float diskRadius = 0.02;
    vec3 sampleOffsets[20] = vec3[](
        vec3( 1,  1,  1), vec3( 1, -1,  1), vec3(-1, -1,  1), vec3(-1,  1,  1),
        vec3( 1,  1, -1), vec3( 1, -1, -1), vec3(-1, -1, -1), vec3(-1,  1, -1),
        vec3( 1,  1,  0), vec3( 1, -1,  0), vec3(-1, -1,  0), vec3(-1,  1,  0),
        vec3( 1,  0,  1), vec3(-1,  0,  1), vec3( 1,  0, -1), vec3(-1,  0, -1),
        vec3( 0,  1,  1), vec3( 0, -1,  1), vec3( 0, -1, -1), vec3( 0,  1, -1)
    );

    int samples = 20;
    for (int s = 0; s < samples; ++s) {
        float closestDepth;
        if (shadowIdx == 0)
            closestDepth = texture(u_PointShadowCubeMaps[0], fragToLight + sampleOffsets[s] * diskRadius).r;
        else
            closestDepth = texture(u_PointShadowCubeMaps[1], fragToLight + sampleOffsets[s] * diskRadius).r;
        closestDepth *= farPlane;
        if (currentDist - bias > closestDepth)
            shadow += 1.0;
    }
    shadow /= float(samples);

    return 1.0 - shadow;
}

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
