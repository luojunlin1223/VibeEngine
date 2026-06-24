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
    float fluidHeight = 0.0;
    if (u_HPFluidDynamicsEnabled == 1) {
        vec3 fluidNormal = SampleFluidNormal(v_FragPos, fluidHeight);
        N = normalize(N + fluidNormal * 0.85);
    }

    float slope = clamp(1.0 - N.y, 0.0, 1.0);
    float heightSignal = clamp(abs(v_FragPos.y - u_HPBaseHeight) / max(abs(u_HPHeightScale), 0.001), 0.0, 1.0);
    float fluidSignal = clamp(abs(fluidHeight) * u_HPFluidHeightScale, 0.0, 1.0);
    float foam = smoothstep(0.32, 0.86, slope + heightSignal * 0.45 + fluidSignal * 0.35) *
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
