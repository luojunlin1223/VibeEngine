// VibeEngine ShaderLab - HPWater first-pass caustic accumulation
//
// HPWater's Unity implementation accumulates caustics through compute shaders,
// water/scene cascade atlases, atomics, optional RGB dispersion, and filtering.
// This pass is the first VibeEngine target in that dataflow: a dedicated
// full-resolution caustic energy texture that the water composite can consume.

Shader "VibeEngine/HPWaterCaustic" {
    Properties {
    }

    SubShader {
        Tags { "RenderType"="Opaque" "Queue"="Overlay" "LightMode"="HPWaterCaustic" }

        Pass {
            Name "HPWaterCausticPass"

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
    vec2 pos = vec2((gl_VertexID & 1) * 2.0, (gl_VertexID & 2) * 1.0);
    v_UV = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
#endif

#ifdef FRAGMENT
layout(location = 0) in vec2 v_UV;
layout(location = 0) out vec4 FragColor;

uniform sampler2D u_HPWaterNormalRoughness;
uniform sampler2D u_HPWaterScatterThickness;
uniform sampler2D u_HPWaterAbsorptionFoam;
uniform sampler2D u_HPWaterDepth;
uniform sampler2D u_HPWaterMask;
uniform sampler2D u_SceneDepth;

uniform vec3 u_LightDir;
uniform vec3 u_LightColor;
uniform float u_LightIntensity;
uniform float u_CausticStrength;
uniform float u_CausticScale;
uniform float u_CausticDepthFade;
uniform int u_CausticRGBDispersion;
uniform float u_CausticDispersionStrength;
uniform float u_NearClip;
uniform float u_FarClip;
uniform int u_HPWaterMaskEnabled;

float LinearizeDepth(float depth) {
    float z = depth * 2.0 - 1.0;
    return (2.0 * u_NearClip * u_FarClip) /
        max(u_FarClip + u_NearClip - z * (u_FarClip - u_NearClip), 0.0001);
}

float Hash21(vec2 p) {
    p = fract(p * vec2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return fract(p.x * p.y);
}

float CausticStrands(vec2 causticUV) {
    float bandA = sin(causticUV.x * 11.0 + causticUV.y * 2.6);
    float bandB = sin(causticUV.y * 9.0 - causticUV.x * 3.2);
    float bandC = sin((causticUV.x + causticUV.y) * 6.5 + Hash21(floor(causticUV)) * 1.2);
    float interference = (bandA + bandB + bandC) * 0.3333;
    return smoothstep(0.42, 0.95, interference);
}

void main() {
    float waterDepth = texture(u_HPWaterDepth, v_UV).r;
    float waterMask = u_HPWaterMaskEnabled == 1
        ? texture(u_HPWaterMask, v_UV).r
        : (waterDepth < 0.9999 ? 1.0 : 0.0);

    if (waterMask < 0.5 || waterDepth >= 0.9999) {
        FragColor = vec4(0.0);
        return;
    }

    float sceneDepth = texture(u_SceneDepth, v_UV).r;
    if (sceneDepth < waterDepth - 0.00005) {
        FragColor = vec4(0.0);
        return;
    }

    vec4 normalRoughness = texture(u_HPWaterNormalRoughness, v_UV);
    vec4 scatterThickness = texture(u_HPWaterScatterThickness, v_UV);
    vec4 absorptionFoam = texture(u_HPWaterAbsorptionFoam, v_UV);

    vec3 N = normalize(normalRoughness.xyz * 2.0 - 1.0);
    vec3 L = normalize(-u_LightDir);
    float sunFacing = clamp(dot(N, L) * 0.5 + 0.5, 0.0, 1.0);

    float waterLinear = LinearizeDepth(waterDepth);
    float sceneLinear = sceneDepth >= 0.9999
        ? waterLinear + max(scatterThickness.a, 0.1)
        : LinearizeDepth(sceneDepth);
    float thickness = max(sceneLinear - waterLinear, 0.0);
    float depthFade = exp(-thickness / max(u_CausticDepthFade, 0.1));

    vec2 refractedOffset = N.xz * (0.65 + max(thickness, 0.0) * 0.02);
    vec2 lightDrift = normalize(L.xz + vec2(0.001)) * 0.17;
    vec2 causticUV = v_UV * max(u_CausticScale, 0.1) + refractedOffset + lightDrift;

    float slopeFocus = smoothstep(0.025, 0.45, length(N.xz));
    float foamOcclusion = 1.0 - clamp(absorptionFoam.a, 0.0, 1.0) * 0.65;
    float sharedEnergy = slopeFocus * sunFacing * depthFade * foamOcclusion;
    sharedEnergy *= clamp(u_CausticStrength, 0.0, 8.0) * max(u_LightIntensity, 0.0);

    float centerStrands = CausticStrands(causticUV);
    vec3 energyRGB = vec3(centerStrands) * sharedEnergy;
    if (u_CausticRGBDispersion == 1) {
        vec2 dispersionAxis = normalize(N.xz * 0.7 + L.xz * 0.3 + vec2(0.001));
        float dispersion = clamp(u_CausticDispersionStrength, 0.0, 2.0);
        dispersion *= 0.18 + clamp(thickness, 0.0, 50.0) * 0.012;
        float redStrands = CausticStrands(causticUV + dispersionAxis * dispersion);
        float blueStrands = CausticStrands(causticUV - dispersionAxis * dispersion);
        energyRGB = vec3(redStrands, centerStrands, blueStrands) * sharedEnergy;
    }

    vec3 absorption = max(absorptionFoam.rgb, vec3(0.0001));
    vec3 waterTint = mix(vec3(1.0), scatterThickness.rgb, 0.24);
    vec3 attenuation = exp(-absorption * (0.45 + thickness * 0.12));
    vec3 caustic = max(u_LightColor, vec3(0.0)) * waterTint * attenuation * energyRGB;
    float energy = max(max(energyRGB.r, energyRGB.g), energyRGB.b);

    FragColor = vec4(caustic, clamp(energy, 0.0, 1.0));
}
#endif

            ENDGLSL
        }
    }

    FallBack "VibeEngine/HPWaterComposite"
}
