// VibeEngine ShaderLab — Water Shader
// Features: animated wave displacement, Fresnel reflection, specular highlights,
// scrolling normal distortion, depth-based edge transparency, foam.

Shader "VibeEngine/Water" {
    Properties {
        _WaterColor ("Water Color", Color) = (0.1, 0.3, 0.5, 0.7)
        _DeepColor ("Deep Color", Color) = (0.02, 0.08, 0.2, 1.0)
        _SpecularColor ("Specular Color", Color) = (1, 1, 1, 1)
        _NormalMap ("Normal Map", 2D) = "bump" {}
        _FoamTex ("Foam Texture", 2D) = "white" {}
        _WaveSpeed ("Wave Speed", Range(0, 5)) = 1.0
        _WaveHeight ("Wave Height", Range(0, 2)) = 0.3
        _Roughness ("Roughness", Range(0, 1)) = 0.1
    }

    SubShader {
        Tags { "RenderType"="Transparent" "Queue"="Transparent" }

        Pass {
            Name "Water"
            Tags { "LightMode"="Lit" }

            Cull Off
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
uniform float u_Time;
uniform float u_WaveSpeed;
uniform float u_WaveHeight;

out vec3 v_WorldPos;
out vec3 v_Normal;
out vec2 v_TexCoord;
out vec3 v_Color;
out float v_WaveDisp;

// Multi-octave Gerstner-like waves
float Wave(vec2 pos, float time) {
    float w = 0.0;
    // Wave 1: large slow
    w += sin(pos.x * 0.3 + time * 0.7) * 0.5;
    w += sin(pos.y * 0.4 + time * 0.5) * 0.4;
    // Wave 2: medium
    w += sin(pos.x * 0.8 - pos.y * 0.6 + time * 1.2) * 0.25;
    w += sin(pos.x * 0.5 + pos.y * 0.9 + time * 0.9) * 0.2;
    // Wave 3: small detail
    w += sin(pos.x * 2.1 + pos.y * 1.8 + time * 2.0) * 0.1;
    w += sin(pos.x * 1.5 - pos.y * 2.3 + time * 1.7) * 0.08;
    return w;
}

vec3 WaveNormal(vec2 pos, float time, float eps) {
    float h  = Wave(pos, time);
    float hx = Wave(pos + vec2(eps, 0.0), time);
    float hz = Wave(pos + vec2(0.0, eps), time);
    vec3 tangentX = vec3(eps, (hx - h) * u_WaveHeight, 0.0);
    vec3 tangentZ = vec3(0.0, (hz - h) * u_WaveHeight, eps);
    return normalize(cross(tangentZ, tangentX));
}

void main() {
    float time = u_Time * u_WaveSpeed;
    vec3 pos = a_Position;

    // Displace Y by wave function
    float wave = Wave(pos.xz, time);
    pos.y += wave * u_WaveHeight;
    v_WaveDisp = wave;

    vec4 worldPos = u_Model * vec4(pos, 1.0);
    v_WorldPos = worldPos.xyz;

    // Compute wave normal
    vec3 waveNormal = WaveNormal(a_Position.xz, time, 0.1);
    v_Normal = normalize(mat3(transpose(inverse(u_Model))) * waveNormal);

    v_TexCoord = a_TexCoord;
    v_Color = a_Color;
    gl_Position = u_MVP * vec4(pos, 1.0);
}
#endif

#ifdef FRAGMENT
in vec3 v_WorldPos;
in vec3 v_Normal;
in vec2 v_TexCoord;
in vec3 v_Color;
in float v_WaveDisp;

// Water properties
uniform vec4 u_WaterColor;
uniform vec4 u_DeepColor;
uniform vec4 u_SpecularColor;
uniform float u_Roughness;
uniform float u_Time;
uniform float u_WaveSpeed;

// Textures
uniform sampler2D u_NormalMap;
uniform int u_HasNormalMap;
uniform sampler2D u_FoamTex;
uniform int u_HasFoamTex;

// Lighting
uniform vec3  u_LightDir;
uniform vec3  u_LightColor;
uniform float u_LightIntensity;
uniform vec3  u_ViewPos;

// Entity color
uniform vec4 u_EntityColor;

// Point lights (max 8)
uniform int u_NumPointLights;
uniform vec3  u_PointLightPositions[8];
uniform vec3  u_PointLightColors[8];
uniform float u_PointLightIntensities[8];
uniform float u_PointLightRanges[8];

// Spot lights (max 4)
uniform int   u_NumSpotLights;
uniform vec3  u_SpotLightPositions[4];
uniform vec3  u_SpotLightDirections[4];
uniform vec3  u_SpotLightColors[4];
uniform float u_SpotLightIntensities[4];
uniform float u_SpotLightRanges[4];
uniform float u_SpotLightInnerCos[4];
uniform float u_SpotLightOuterCos[4];

out vec4 FragColor;

void main() {
    vec3 N = normalize(v_Normal);
    vec3 V = normalize(u_ViewPos - v_WorldPos);
    vec3 L = normalize(u_LightDir);

    // Scrolling normal map distortion
    if (u_HasNormalMap == 1) {
        float t = u_Time * u_WaveSpeed * 0.3;
        vec2 uv1 = v_WorldPos.xz * 0.1 + vec2(t * 0.3, t * 0.2);
        vec2 uv2 = v_WorldPos.xz * 0.15 + vec2(-t * 0.2, t * 0.25);
        vec3 n1 = texture(u_NormalMap, uv1).rgb * 2.0 - 1.0;
        vec3 n2 = texture(u_NormalMap, uv2).rgb * 2.0 - 1.0;
        vec3 detailNormal = normalize(vec3(
            (n1.x + n2.x) * 0.5,
            1.0,
            (n1.y + n2.y) * 0.5 // RG normal map convention
        ));
        // Blend with wave normal using TBN
        N = normalize(N + detailNormal * 0.3);
    }

    // ── Fresnel ──────────────────────────────────────────────────
    float NdotV = max(dot(N, V), 0.0);
    float fresnel = pow(1.0 - NdotV, 4.0); // Schlick approximation (water IOR ~1.33)
    fresnel = clamp(fresnel, 0.02, 0.98);

    // ── Water color blend (deep vs shallow via wave displacement) ─
    float depthFactor = clamp(v_WaveDisp * 0.5 + 0.5, 0.0, 1.0);
    vec3 waterCol = mix(u_DeepColor.rgb, u_WaterColor.rgb, depthFactor);

    // Reflection approximation (sky color blend via fresnel)
    vec3 skyReflect = vec3(0.5, 0.7, 1.0) * u_LightColor; // simple sky approximation
    vec3 baseColor = mix(waterCol, skyReflect, fresnel * 0.6);

    // ── Specular (Blinn-Phong) ───────────────────────────────────
    vec3 H = normalize(L + V);
    float shininess = max(2.0 / (u_Roughness * u_Roughness) - 2.0, 1.0);
    shininess = clamp(shininess, 1.0, 2048.0);
    float spec = pow(max(dot(N, H), 0.0), shininess);
    vec3 specular = u_SpecularColor.rgb * u_LightColor * u_LightIntensity * spec;

    // Sun highlight on water (enhanced for water surfaces)
    float sunGlint = pow(max(dot(N, H), 0.0), shininess * 4.0) * u_LightIntensity;
    specular += u_LightColor * sunGlint * 0.5;

    // ── Diffuse ──────────────────────────────────────────────────
    float NdotL = max(dot(N, L), 0.0);
    vec3 diffuse = baseColor * NdotL * u_LightColor * u_LightIntensity * 0.4;

    // Ambient
    vec3 ambient = baseColor * 0.25;

    // ── Point lights ─────────────────────────────────────────────
    for (int i = 0; i < u_NumPointLights; i++) {
        vec3 plDir = u_PointLightPositions[i] - v_WorldPos;
        float dist = length(plDir);
        if (dist > u_PointLightRanges[i]) continue;
        plDir /= dist;
        float atten = 1.0 - smoothstep(0.0, u_PointLightRanges[i], dist);
        atten *= atten;
        float plSpec = pow(max(dot(N, normalize(plDir + V)), 0.0), shininess);
        diffuse += baseColor * max(dot(N, plDir), 0.0) * u_PointLightColors[i] * u_PointLightIntensities[i] * atten * 0.4;
        specular += u_SpecularColor.rgb * u_PointLightColors[i] * plSpec * atten;
    }

    // ── Spot lights ──────────────────────────────────────────────
    for (int i = 0; i < u_NumSpotLights; i++) {
        vec3 slDir = u_SpotLightPositions[i] - v_WorldPos;
        float dist = length(slDir);
        if (dist > u_SpotLightRanges[i]) continue;
        slDir /= dist;
        float theta   = dot(slDir, normalize(-u_SpotLightDirections[i]));
        float epsilon = u_SpotLightInnerCos[i] - u_SpotLightOuterCos[i];
        float spotAtt = clamp((theta - u_SpotLightOuterCos[i]) / max(epsilon, 0.001), 0.0, 1.0);
        if (spotAtt <= 0.0) continue;
        float atten = 1.0 - smoothstep(0.0, u_SpotLightRanges[i], dist);
        atten *= atten * spotAtt;
        float slSpec = pow(max(dot(N, normalize(slDir + V)), 0.0), shininess);
        diffuse += baseColor * max(dot(N, slDir), 0.0) * u_SpotLightColors[i] * u_SpotLightIntensities[i] * atten * 0.4;
        specular += u_SpecularColor.rgb * u_SpotLightColors[i] * slSpec * atten;
    }

    // ── Foam ─────────────────────────────────────────────────────
    vec3 foam = vec3(0.0);
    if (u_HasFoamTex == 1) {
        float t = u_Time * u_WaveSpeed * 0.2;
        vec2 foamUV = v_WorldPos.xz * 0.2 + vec2(t * 0.1, -t * 0.15);
        float foamSample = texture(u_FoamTex, foamUV).r;
        // Foam appears on wave crests
        float foamMask = smoothstep(0.3, 0.6, v_WaveDisp);
        foam = vec3(foamSample * foamMask * 0.5);
    }

    vec3 finalColor = ambient + diffuse + specular + foam;
    finalColor *= u_EntityColor.rgb * v_Color;

    // Alpha: controlled by water color alpha + fresnel (more opaque at glancing angles)
    float alpha = u_WaterColor.a * u_EntityColor.a;
    alpha = mix(alpha, 1.0, fresnel * 0.5);

    FragColor = vec4(finalColor, alpha);
}
#endif

            ENDGLSL
        }
    }

    FallBack "VibeEngine/Unlit"
}
