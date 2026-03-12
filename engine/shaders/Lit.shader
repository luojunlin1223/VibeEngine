// VibeEngine ShaderLab — Lit Shader (PBR + CSM Shadows)
// Used for 3D meshes (Cube, imported FBX) with physically-based lighting
// and Cascaded Shadow Maps with PCF soft shadows.

Shader "VibeEngine/Lit" {
    Properties {
        _MainTex ("Main Texture", 2D) = "white" {}
        _EntityColor ("Entity Color", Color) = (1, 1, 1, 1)
        _Metallic ("Metallic", Range(0, 1)) = 0.0
        _Roughness ("Roughness", Range(0, 1)) = 0.5
        _AO ("Ambient Occlusion", Range(0, 1)) = 1.0
    }

    SubShader {
        Tags { "RenderType"="Opaque" "Queue"="Geometry" "LightMode"="Forward" }

        Pass {
            Name "ForwardLit"
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

uniform vec3  u_LightDir;
uniform vec3  u_LightColor;
uniform float u_LightIntensity;
uniform vec3  u_ViewPos;
uniform vec4  u_EntityColor;
uniform float u_Metallic;
uniform float u_Roughness;
uniform float u_AO;
uniform sampler2D u_Texture;
uniform int   u_UseTexture;

// Shadow uniforms
uniform int   u_ShadowEnabled;
uniform sampler2DArrayShadow u_ShadowMap;
uniform mat4  u_LightSpaceMatrices[3];
uniform vec3  u_CascadeSplits;
uniform mat4  u_ViewMatrix;
uniform float u_ShadowBias;
uniform float u_ShadowNormalBias;
uniform int   u_PCFRadius;

out vec4 FragColor;

const float PI = 3.14159265359;

// ── PBR Functions ────────────────────────────────────────────────────

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
        return 1.0; // no shadow

    // Determine cascade by view-space depth
    vec4 fragPosViewSpace = u_ViewMatrix * vec4(fragPos, 1.0);
    float depthValue = -fragPosViewSpace.z; // positive depth in view space

    int cascade = 2;
    if (depthValue < u_CascadeSplits.x)
        cascade = 0;
    else if (depthValue < u_CascadeSplits.y)
        cascade = 1;

    // Apply normal bias: push position along normal to reduce shadow acne
    vec3 biasedPos = fragPos + normal * u_ShadowNormalBias * (1.0 + float(cascade));

    // Transform to light space
    vec4 fragPosLightSpace = u_LightSpaceMatrices[cascade] * vec4(biasedPos, 1.0);
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords = projCoords * 0.5 + 0.5; // [-1,1] -> [0,1]

    // Outside shadow map bounds → no shadow
    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0 ||
        projCoords.z > 1.0)
        return 1.0;

    // Slope-based bias
    float cosTheta = max(dot(normal, lightDir), 0.0);
    float bias = u_ShadowBias * (1.0 - cosTheta);
    float currentDepth = projCoords.z - bias;

    // PCF (Percentage Closer Filtering)
    float shadow = 0.0;
    vec2 texelSize = 1.0 / vec2(textureSize(u_ShadowMap, 0).xy);

    int samples = 0;
    for (int x = -u_PCFRadius; x <= u_PCFRadius; ++x) {
        for (int y = -u_PCFRadius; y <= u_PCFRadius; ++y) {
            vec2 offset = vec2(float(x), float(y)) * texelSize;
            // sampler2DArrayShadow: texture(sampler, vec4(uv, layer, compareRef))
            shadow += texture(u_ShadowMap, vec4(projCoords.xy + offset, float(cascade), currentDepth));
            samples++;
        }
    }
    shadow /= float(samples);

    return shadow; // 0.0 = fully in shadow, 1.0 = fully lit
}

// ── Main ─────────────────────────────────────────────────────────────

void main() {
    vec3 albedo = v_Color;
    if (u_UseTexture == 1)
        albedo = texture(u_Texture, v_TexCoord).rgb;
    albedo *= u_EntityColor.rgb;

    float metallic  = u_Metallic;
    float roughness = max(u_Roughness, 0.04);
    float ao        = u_AO;

    vec3 N = normalize(v_Normal);
    vec3 V = normalize(u_ViewPos - v_FragPos);

    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    vec3 L = normalize(u_LightDir);
    vec3 H = normalize(V + L);
    vec3 radiance = u_LightColor * u_LightIntensity;

    float NDF = DistributionGGX(N, H, roughness);
    float G   = GeometrySmith(N, V, L, roughness);
    vec3  F   = FresnelSchlick(max(dot(H, V), 0.0), F0);

    vec3  specular = (NDF * G * F) / max(4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0), 0.001);
    vec3  kD = (vec3(1.0) - F) * (1.0 - metallic);

    float NdotL = max(dot(N, L), 0.0);

    // Shadow factor
    float shadow = ShadowCalculation(v_FragPos, N, L);

    vec3 Lo = (kD * albedo / PI + specular) * radiance * NdotL * shadow;

    vec3 ambient = vec3(0.03) * albedo * ao;
    vec3 color = ambient + Lo;

    // Tone mapping + gamma
    color = color / (color + vec3(1.0));
    color = pow(color, vec3(1.0 / 2.2));

    FragColor = vec4(color, u_EntityColor.a);
}
#endif

            ENDGLSL
        }
    }

    FallBack "VibeEngine/Unlit"
}
