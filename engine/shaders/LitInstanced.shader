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

// Lighting
uniform vec3  u_LightDir;
uniform vec3  u_LightColor;
uniform float u_LightIntensity;
uniform vec3  u_ViewPos;

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

// Reflection probe uniforms
uniform int          u_HasReflectionProbe;
uniform samplerCube  u_ReflectionProbe;
uniform float        u_ReflectionIntensity;

out vec4 FragColor;

const float PI = 3.14159265359;

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

vec3 FresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

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

float ShadowCalculation(vec3 fragPos, vec3 normal, vec3 lightDir) {
    if (u_ShadowEnabled == 0) return 1.0;

    vec4 fragPosViewSpace = u_ViewMatrix * vec4(fragPos, 1.0);
    float depthValue = -fragPosViewSpace.z;

    int cascade = 2;
    if (depthValue < u_CascadeSplits.x) cascade = 0;
    else if (depthValue < u_CascadeSplits.y) cascade = 1;

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

// ── Spot Light Shadow Calculation ────────────────────────────────────

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

// ── Point Light Shadow Calculation ──────────────────────────────────

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
