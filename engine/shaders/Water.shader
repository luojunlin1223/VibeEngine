// VibeEngine ShaderLab - HPWater-inspired surface shader.
// This is the visible surface slice of the HPWater pipeline: dynamic wave
// normals, scatter/absorption color, Fresnel reflection, specular glints, and
// height/slope foam. Dedicated water GBuffer/refraction passes can be attached
// to the same HPWaterComponent data later.

Shader "VibeEngine/Water" {
    Properties {
        _WaterColor ("Water Color", Color) = (0.07, 0.42, 0.58, 0.72)
        _DeepColor ("Deep Color", Color) = (0.005, 0.025, 0.055, 1.0)
        _SpecularColor ("Specular Color", Color) = (1, 1, 1, 1)
        _Roughness ("Roughness", Range(0, 1)) = 0.06
    }

    SubShader {
        Tags { "RenderType"="Transparent" "Queue"="Transparent" }

        Pass {
            Name "HPWaterSurface"
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

out vec3 v_WorldPos;
out vec3 v_Normal;
out vec3 v_Color;
out vec2 v_TexCoord;
out float v_LocalHeight;

void main() {
    vec4 worldPos = u_Model * vec4(a_Position, 1.0);
    v_WorldPos = worldPos.xyz;
    v_Normal = normalize(mat3(transpose(inverse(u_Model))) * a_Normal);
    v_Color = a_Color;
    v_TexCoord = a_TexCoord;
    v_LocalHeight = a_Position.y;
    gl_Position = u_MVP * vec4(a_Position, 1.0);
}
#endif

#ifdef FRAGMENT
in vec3 v_WorldPos;
in vec3 v_Normal;
in vec3 v_Color;
in vec2 v_TexCoord;
in float v_LocalHeight;

uniform vec4 u_WaterColor;
uniform vec4 u_DeepColor;
uniform vec4 u_SpecularColor;
uniform float u_Roughness;

uniform vec4 u_EntityColor;
uniform vec3 u_LightDir;
uniform vec3 u_LightColor;
uniform float u_LightIntensity;
uniform vec3 u_ViewPos;

uniform int u_IndirectLightingEnabled;
uniform vec3 u_IndirectSkyColor;
uniform vec3 u_IndirectGroundColor;
uniform vec3 u_IndirectTint;
uniform float u_IndirectDiffuseIntensity;
uniform float u_SkyReflectionIntensity;

uniform int u_HPWaterEnabled;
uniform vec3 u_HPScatterColor;
uniform vec3 u_HPAbsorptionColor;
uniform vec3 u_HPFoamColor;
uniform float u_HPFoamIntensity;
uniform float u_HPRoughness;
uniform float u_HPRefractionStrength;
uniform float u_HPDepthTintDistance;
uniform float u_HPHeightScale;

out vec4 FragColor;

const float PI = 3.14159265359;

vec3 FresnelSchlick(float cosTheta, vec3 f0) {
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 SampleSkyGradient(vec3 dir) {
    float t = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
    return mix(u_IndirectGroundColor, u_IndirectSkyColor, t) * u_IndirectTint;
}

float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float nDotH = max(dot(N, H), 0.0);
    float denom = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 0.000001);
}

float GeometrySchlickGGX(float nDotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return nDotV / max(nDotV * (1.0 - k) + k, 0.000001);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    return GeometrySchlickGGX(max(dot(N, V), 0.0), roughness) *
           GeometrySchlickGGX(max(dot(N, L), 0.0), roughness);
}

void main() {
    vec3 N = normalize(v_Normal);
    vec3 V = normalize(u_ViewPos - v_WorldPos);
    vec3 L = normalize(u_LightDir);
    vec3 H = normalize(V + L);

    vec3 scatterColor = (u_HPWaterEnabled == 1) ? u_HPScatterColor : u_WaterColor.rgb;
    vec3 absorptionColor = (u_HPWaterEnabled == 1) ? u_HPAbsorptionColor : u_DeepColor.rgb;
    vec3 foamColor = (u_HPWaterEnabled == 1) ? u_HPFoamColor : vec3(0.9, 0.96, 1.0);
    float roughness = clamp((u_HPWaterEnabled == 1) ? u_HPRoughness : u_Roughness, 0.015, 0.75);
    float foamIntensity = (u_HPWaterEnabled == 1) ? u_HPFoamIntensity : 0.2;
    float depthTintDistance = max((u_HPWaterEnabled == 1) ? u_HPDepthTintDistance : 10.0, 0.001);

    float nDotV = max(dot(N, V), 0.0);
    vec3 F0 = vec3(0.0204); // water IOR ~1.33
    vec3 fresnel = FresnelSchlick(nDotV, F0);

    float slope = clamp(1.0 - N.y, 0.0, 1.0);
    float heightSignal = clamp(abs(v_LocalHeight) / max(abs(u_HPHeightScale), 0.001), 0.0, 1.0);
    float foam = smoothstep(0.30, 0.82, slope + heightSignal * 0.45) * foamIntensity;

    float opticalDepth = mix(1.0, depthTintDistance, clamp(1.0 - nDotV, 0.0, 1.0));
    vec3 transmittance = exp(-absorptionColor * opticalDepth);
    vec3 bodyColor = scatterColor * (1.0 - transmittance) + u_DeepColor.rgb * transmittance;

    float nDotL = max(dot(N, L), 0.0);
    vec3 lightRadiance = u_LightColor * u_LightIntensity;
    vec3 subsurface = scatterColor * lightRadiance * (0.22 + 0.55 * nDotL);

    float D = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    vec3 specular = D * G * fresnel / max(4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0), 0.001);
    specular *= lightRadiance * nDotL * u_SpecularColor.rgb;

    vec3 skyReflection = SampleSkyGradient(reflect(-V, N)) * fresnel * u_SkyReflectionIntensity;
    vec3 indirect = vec3(0.0);
    if (u_IndirectLightingEnabled == 1) {
        indirect = bodyColor * SampleSkyGradient(N) * u_IndirectDiffuseIntensity * 0.35;
    }

    vec3 refractionApprox = mix(bodyColor, scatterColor, clamp(u_HPRefractionStrength, 0.0, 1.0));
    vec3 color = refractionApprox + subsurface + specular + skyReflection + indirect;
    color = mix(color, foamColor, clamp(foam, 0.0, 1.0));
    color *= u_EntityColor.rgb * v_Color;

    float alpha = clamp(u_WaterColor.a * u_EntityColor.a + fresnel.r * 0.45 + foam * 0.25, 0.25, 0.95);
    FragColor = vec4(color, alpha);
}
#endif

            ENDGLSL
        }
    }

    FallBack "VibeEngine/Unlit"
}
