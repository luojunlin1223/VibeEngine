// VibeEngine ShaderLab — Terrain shader with height-based texture splatting.
// Blends up to 4 texture layers based on vertex height + slope.
// Uses PBR lighting from Lit shader foundations.

Shader "VibeEngine/Terrain" {
    Properties {
        _Layer0 ("Layer 0 (Low)", 2D) = "white" {}
        _Layer1 ("Layer 1 (Mid-Low)", 2D) = "white" {}
        _Layer2 ("Layer 2 (Mid-High)", 2D) = "white" {}
        _Layer3 ("Layer 3 (High)", 2D) = "white" {}
        _Roughness ("Roughness", Range(0, 1)) = 0.8
    }

    SubShader {
        Tags { "RenderType"="Opaque" "Queue"="Geometry" }

        Pass {
            Name "TerrainLit"
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

out vec3 v_Normal;
out vec3 v_FragPos;
out vec2 v_TexCoord;
out vec3 v_Color;
out float v_Height;
out float v_Slope;

void main() {
    vec4 worldPos = u_Model * vec4(a_Position, 1.0);
    v_FragPos  = worldPos.xyz;
    v_Normal   = normalize(mat3(transpose(inverse(u_Model))) * a_Normal);
    v_TexCoord = a_TexCoord;
    v_Color    = a_Color;
    v_Height   = a_Position.y; // local height for splatting
    v_Slope    = 1.0 - abs(normalize(v_Normal).y); // 0=flat, 1=vertical
    gl_Position = u_MVP * vec4(a_Position, 1.0);
}
#endif

#ifdef FRAGMENT
in vec3 v_Normal;
in vec3 v_FragPos;
in vec2 v_TexCoord;
in vec3 v_Color;
in float v_Height;
in float v_Slope;

// Terrain layers
uniform sampler2D u_Layer0;
uniform sampler2D u_Layer1;
uniform sampler2D u_Layer2;
uniform sampler2D u_Layer3;

// Layer blend heights (normalized terrain height 0-1)
uniform float u_BlendHeight0; // layer0 -> layer1 transition
uniform float u_BlendHeight1; // layer1 -> layer2 transition
uniform float u_BlendHeight2; // layer2 -> layer3 transition
uniform float u_HeightScale;  // world height scale for normalization

// Tiling
uniform float u_Tiling0;
uniform float u_Tiling1;
uniform float u_Tiling2;
uniform float u_Tiling3;

// Lighting
uniform vec3  u_LightDir;
uniform vec3  u_LightColor;
uniform float u_LightIntensity;
uniform vec3  u_ViewPos;
uniform float u_Roughness;

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

float SmoothBlend(float t, float edge, float width) {
    return smoothstep(edge - width, edge + width, t);
}

void main() {
    // Normalize height for splatting (0-1 range)
    float normHeight = clamp(v_Height / max(u_HeightScale, 0.01), 0.0, 1.0);

    // Sample all layers with tiled UVs
    vec2 worldUV = v_FragPos.xz; // world-space tiling
    vec3 c0 = texture(u_Layer0, worldUV * u_Tiling0).rgb;
    vec3 c1 = texture(u_Layer1, worldUV * u_Tiling1).rgb;
    vec3 c2 = texture(u_Layer2, worldUV * u_Tiling2).rgb;
    vec3 c3 = texture(u_Layer3, worldUV * u_Tiling3).rgb;

    // Height-based blending
    float blendWidth = 0.05;
    float w1 = SmoothBlend(normHeight, u_BlendHeight0, blendWidth);
    float w2 = SmoothBlend(normHeight, u_BlendHeight1, blendWidth);
    float w3 = SmoothBlend(normHeight, u_BlendHeight2, blendWidth);

    vec3 albedo = mix(mix(mix(c0, c1, w1), c2, w2), c3, w3);

    // Slope-based rock blending: steep areas get more rock (layer2)
    float slopeBlend = smoothstep(0.4, 0.7, v_Slope);
    albedo = mix(albedo, c2, slopeBlend * 0.6);

    albedo *= v_Color * u_EntityColor.rgb;

    // ── Lighting (simplified PBR) ────────────────────────────────
    vec3 N = normalize(v_Normal);
    vec3 V = normalize(u_ViewPos - v_FragPos);
    vec3 L = normalize(u_LightDir);

    // Diffuse
    float NdotL = max(dot(N, L), 0.0);
    vec3 diffuse = albedo * NdotL * u_LightColor * u_LightIntensity;

    // Specular (Blinn-Phong)
    vec3 H = normalize(L + V);
    float roughness = max(u_Roughness, 0.04);
    float shininess = (2.0 / (roughness * roughness) - 2.0);
    shininess = clamp(shininess, 1.0, 256.0);
    float spec = pow(max(dot(N, H), 0.0), shininess);
    vec3 specular = u_LightColor * u_LightIntensity * spec * 0.2;

    // Ambient
    vec3 ambient = albedo * 0.15;

    // Point lights
    for (int i = 0; i < u_NumPointLights; i++) {
        vec3 plDir = u_PointLightPositions[i] - v_FragPos;
        float dist = length(plDir);
        if (dist > u_PointLightRanges[i]) continue;
        plDir /= dist;
        float atten = 1.0 - smoothstep(0.0, u_PointLightRanges[i], dist);
        atten *= atten;
        float plNdotL = max(dot(N, plDir), 0.0);
        diffuse += albedo * plNdotL * u_PointLightColors[i] * u_PointLightIntensities[i] * atten;
    }

    // Spot lights
    for (int i = 0; i < u_NumSpotLights; i++) {
        vec3 slDir = u_SpotLightPositions[i] - v_FragPos;
        float dist = length(slDir);
        if (dist > u_SpotLightRanges[i]) continue;
        slDir /= dist;
        float theta   = dot(slDir, normalize(-u_SpotLightDirections[i]));
        float epsilon = u_SpotLightInnerCos[i] - u_SpotLightOuterCos[i];
        float spotAtt = clamp((theta - u_SpotLightOuterCos[i]) / max(epsilon, 0.001), 0.0, 1.0);
        if (spotAtt <= 0.0) continue;
        float atten = 1.0 - smoothstep(0.0, u_SpotLightRanges[i], dist);
        atten *= atten * spotAtt;
        float slNdotL = max(dot(N, slDir), 0.0);
        diffuse += albedo * slNdotL * u_SpotLightColors[i] * u_SpotLightIntensities[i] * atten;
    }

    vec3 color = ambient + diffuse + specular;
    FragColor = vec4(color, 1.0);
}
#endif

            ENDGLSL
        }
    }

    FallBack "VibeEngine/Lit"
}
