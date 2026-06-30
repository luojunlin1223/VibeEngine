// VibeEngine ShaderLab - HPWater surface data pass
// Writes the dedicated HPWater G-buffer used by refraction, volumetric water
// lighting, caustics, and fluid interaction passes.

Shader "VibeEngine/HPWaterGBuffer" {
    Properties {
    }

    SubShader {
        Tags { "RenderType"="Opaque" "Queue"="Geometry" "LightMode"="HPWaterGBuffer" }

        Pass {
            Name "HPWaterGBufferPass"

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

void main() {
    v_Normal = normalize(mat3(u_Model) * a_Normal);
    v_FragPos = vec3(u_Model * vec4(a_Position, 1.0));
    v_TexCoord = a_TexCoord;
    gl_Position = u_MVP * vec4(a_Position, 1.0);
}
#endif

#ifdef FRAGMENT
in vec3 v_Normal;
in vec3 v_FragPos;
in vec2 v_TexCoord;

uniform vec3  u_HPScatterColor;
uniform vec3  u_HPAbsorptionColor;
uniform vec3  u_HPFoamColor;
uniform float u_HPFoamIntensity;
uniform float u_HPRoughness;
uniform float u_HPThickness;
uniform float u_HPHeightScale;
uniform float u_HPBaseHeight;
uniform int   u_HPSpectrumWaves;
uniform float u_HPSpectrumAmplitude;
uniform float u_HPSpectrumWindAngle;
uniform float u_HPSpectrumWindSpeed;
uniform float u_HPSpectrumDirectionalSpread;
uniform float u_HPSpectrumSwell;
uniform float u_HPSpectrumShortWaveFade;
uniform float u_HPSpectrumTime;
uniform float u_HPSpectrumNormalStrength;
uniform float u_HPChoppiness;
uniform sampler2D u_HPSpectrumTexture;
uniform int   u_HPSpectrumTextureEnabled;
uniform sampler2D u_HPFluidHeightTexture;
uniform int   u_HPFluidDynamicsEnabled;
uniform vec3  u_HPFluidBoxCenter;
uniform vec3  u_HPFluidBoxSize;
uniform float u_HPFluidHeightScale;

layout(location = 0) out vec4 gWaterNormalRoughness;
layout(location = 1) out vec4 gWaterScatterThickness;
layout(location = 2) out vec4 gWaterAbsorptionFoam;

vec2 WorldToFluidUV(vec3 worldPos) {
    vec2 boxSize = max(abs(u_HPFluidBoxSize.xz), vec2(0.001));
    return (worldPos.xz - u_HPFluidBoxCenter.xz) / boxSize + vec2(0.5);
}

float SampleFluidHeight(vec2 uv) {
    return texture(u_HPFluidHeightTexture, clamp(uv, vec2(0.0), vec2(1.0))).r;
}

float SpectrumWave(vec2 localXZ, vec2 dir, float wavelength, float amplitude, float phaseOffset,
                   out vec2 gradient, out vec2 chop) {
    const float PI = 3.14159265358979323846;
    const float TWO_PI = PI * 2.0;
    const float GRAVITY = 9.81;
    float k = TWO_PI / max(wavelength, 0.25);
    float omega = sqrt(GRAVITY * k);
    float phase = k * dot(dir, localXZ) + omega * u_HPSpectrumTime + phaseOffset;
    float s = sin(phase);
    float c = cos(phase);
    gradient = dir * (c * amplitude * k);
    chop = dir * (c * amplitude * u_HPChoppiness);
    return s * amplitude;
}

vec3 SampleSpectrumNormal(vec3 worldPos, out float spectrumHeightSignal) {
    spectrumHeightSignal = 0.0;
    if (u_HPSpectrumWaves != 1 || u_HPSpectrumAmplitude <= 0.0) {
        return vec3(0.0, 1.0, 0.0);
    }

    float windRad = radians(u_HPSpectrumWindAngle);
    vec2 wind = normalize(vec2(cos(windRad), sin(windRad)));
    vec2 side = vec2(-wind.y, wind.x);
    vec2 localXZ = worldPos.xz - u_HPFluidBoxCenter.xz;
    float domain = max(max(abs(u_HPFluidBoxSize.x), abs(u_HPFluidBoxSize.z)), 1.0);
    float windSpeed = clamp(u_HPSpectrumWindSpeed, 0.1, 80.0);
    float windEnergy = clamp(pow(windSpeed / 12.0, 0.65), 0.35, 3.0);
    float directionalSpread = clamp(u_HPSpectrumDirectionalSpread, 0.0, 1.0);
    float spreadPower = mix(0.75, 6.0, directionalSpread);
    float spreadFloor = mix(0.42, 0.08, directionalSpread);
    float swellDecay = mix(0.52, 0.22, clamp(u_HPSpectrumSwell, 0.0, 1.0));
    float shortWaveFade = clamp(u_HPSpectrumShortWaveFade, 0.0, 2.0);
    vec2 totalGradient = vec2(0.0);
    float totalHeight = 0.0;

    const float wavelengthFactors[16] = float[16](
        0.46, 0.32, 0.22, 0.155, 0.108, 0.076, 0.054, 0.038,
        0.027, 0.019, 0.0135, 0.0095, 0.0068, 0.0048, 0.0034, 0.0024);
    const float directionOffsets[16] = float[16](
        0.00, 0.16, -0.21, 0.34, -0.43, 0.56, -0.69, 0.82,
        -0.96, 1.10, -1.26, 1.42, -1.58, 1.74, -1.92, 2.10);
    const float phaseOffsets[16] = float[16](
        0.0, 1.7, 3.1, 4.6, 2.4, 5.2, 0.9, 3.9,
        5.8, 2.8, 4.2, 1.2, 3.6, 5.0, 0.45, 2.15);

    for (int i = 0; i < 16; ++i) {
        vec2 dir = normalize(wind + side * directionOffsets[i]);
        float octave = float(i);
        float swell = exp(-octave * swellDecay);
        float capillaryFade = 1.0 / (1.0 + pow(max(octave - 9.0, 0.0), 1.35) * shortWaveFade);
        float directionalEnergy = pow(max(dot(dir, wind), 0.0), spreadPower) * (1.0 - spreadFloor) + spreadFloor;
        float amplitude = u_HPSpectrumAmplitude * windEnergy * swell * capillaryFade * directionalEnergy;
        float wavelength = max(domain * wavelengthFactors[i], 0.25);
        vec2 gradient;
        vec2 chop;
        totalHeight += SpectrumWave(localXZ, dir, wavelength, amplitude, phaseOffsets[i], gradient, chop);
        totalGradient += gradient * u_HPSpectrumNormalStrength * (1.15 + octave * 0.045);
    }

    spectrumHeightSignal = clamp(abs(totalHeight) / max(abs(u_HPSpectrumAmplitude) * 2.5, 0.001), 0.0, 1.0);
    return normalize(vec3(-totalGradient.x, 1.0, -totalGradient.y));
}

vec3 SampleFluidNormal(vec3 worldPos, out float centerHeight) {
    ivec2 textureSizePx = textureSize(u_HPFluidHeightTexture, 0);
    vec2 texel = 1.0 / vec2(max(textureSizePx, ivec2(1)));
    vec2 uv = WorldToFluidUV(worldPos);

    centerHeight = SampleFluidHeight(uv);
    float hLeft = SampleFluidHeight(uv - vec2(texel.x, 0.0));
    float hRight = SampleFluidHeight(uv + vec2(texel.x, 0.0));
    float hDown = SampleFluidHeight(uv - vec2(0.0, texel.y));
    float hUp = SampleFluidHeight(uv + vec2(0.0, texel.y));

    vec2 worldTexel = max(abs(u_HPFluidBoxSize.xz) / vec2(max(textureSizePx, ivec2(1))), vec2(0.001));
    float dX = (hLeft - hRight) * u_HPFluidHeightScale / (worldTexel.x * 2.0);
    float dZ = (hDown - hUp) * u_HPFluidHeightScale / (worldTexel.y * 2.0);
    return normalize(vec3(dX, 1.0, dZ));
}

void main() {
    vec3 N = normalize(v_Normal);
    float roughness = clamp(u_HPRoughness, 0.015, 0.75);
    float spectrumHeightSignal = 0.0;
    if (u_HPSpectrumWaves == 1) {
        vec3 spectrumNormal;
        if (u_HPSpectrumTextureEnabled == 1) {
            vec2 spectrumUV = WorldToFluidUV(v_FragPos);
            vec4 spectrumPayload = texture(u_HPSpectrumTexture, clamp(spectrumUV, vec2(0.0), vec2(1.0)));
            spectrumNormal = normalize(spectrumPayload.rgb * 2.0 - 1.0);
            spectrumHeightSignal = clamp(spectrumPayload.a, 0.0, 1.0);
        } else {
            spectrumNormal = SampleSpectrumNormal(v_FragPos, spectrumHeightSignal);
        }
        float spectrumBlend = clamp(u_HPSpectrumNormalStrength, 0.0, 3.0) * 0.65;
        N = normalize(mix(N, spectrumNormal, clamp(spectrumBlend, 0.0, 0.95)));
    }

    float fluidHeight = 0.0;
    if (u_HPFluidDynamicsEnabled == 1) {
        vec3 fluidNormal = SampleFluidNormal(v_FragPos, fluidHeight);
        N = normalize(N + fluidNormal * 0.85);
    }

    float slope = clamp(1.0 - N.y, 0.0, 1.0);
    float heightSignal = clamp(abs(v_FragPos.y - u_HPBaseHeight) / max(abs(u_HPHeightScale), 0.001), 0.0, 1.0);
    float fluidSignal = clamp(abs(fluidHeight) * u_HPFluidHeightScale, 0.0, 1.0);
    float foam = smoothstep(0.32, 0.86, slope + heightSignal * 0.45 + spectrumHeightSignal * 0.28 + fluidSignal * 0.35) *
        clamp(u_HPFoamIntensity, 0.0, 2.0);

    vec3 encodedNormal = N * 0.5 + 0.5;
    gWaterNormalRoughness = vec4(encodedNormal, roughness);
    gWaterScatterThickness = vec4(max(u_HPScatterColor, vec3(0.0)), max(u_HPThickness, 0.0));
    gWaterAbsorptionFoam = vec4(max(u_HPAbsorptionColor, vec3(0.0)), clamp(foam, 0.0, 1.0));
}
#endif

            ENDGLSL
        }
    }

    FallBack "VibeEngine/GBuffer"
}
