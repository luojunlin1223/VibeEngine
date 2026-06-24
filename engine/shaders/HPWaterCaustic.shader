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
uniform sampler2D u_HPWaterCausticAtlas;
uniform sampler2D u_HPWaterCausticAtlasDepth;
uniform sampler2D u_HPWaterCausticComputeIrradiance;

uniform vec3 u_LightDir;
uniform vec3 u_LightColor;
uniform float u_LightIntensity;
uniform float u_CausticStrength;
uniform float u_CausticScale;
uniform float u_CausticDepthFade;
uniform float u_CausticTransmittanceStrength;
uniform float u_CausticLeakReduction;
uniform float u_CausticScatterBoost;
uniform int u_CausticRGBDispersion;
uniform float u_CausticDispersionStrength;
uniform float u_NearClip;
uniform float u_FarClip;
uniform int u_HPWaterMaskEnabled;
uniform int u_HPWaterCausticAtlasEnabled;
uniform int u_HPWaterCausticComputeEnabled;
uniform float u_HPWaterCausticAtlasWidth;
uniform float u_HPWaterCausticAtlasHeight;
uniform mat4 u_InverseViewProjection;
uniform mat4 u_WaterCascadeVP[4];
uniform float u_WaterCascadeSplits[4];

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

vec3 ReconstructWorldPosition(vec2 uv, float depth) {
    vec2 ndcXY = uv * 2.0 - 1.0;
    float ndcZ = depth * 2.0 - 1.0;
    vec4 world = u_InverseViewProjection * vec4(ndcXY, ndcZ, 1.0);
    float invW = abs(world.w) > 0.00001 ? 1.0 / world.w : 0.0;
    return world.xyz * invW;
}

int SelectWaterCascade(float viewDepth) {
    for (int i = 0; i < 4; ++i) {
        if (viewDepth <= u_WaterCascadeSplits[i])
            return i;
    }
    return 3;
}

vec2 LocalToAtlasUV(vec2 localUV, int cascadeIndex) {
    vec2 tile = vec2(float(cascadeIndex % 2), float(cascadeIndex / 2));
    return (tile + localUV) * 0.5;
}

vec2 AtlasTileHalfTexel() {
    vec2 tileSize = max(vec2(u_HPWaterCausticAtlasWidth, u_HPWaterCausticAtlasHeight) * 0.5, vec2(1.0));
    return 0.5 / tileSize;
}

float AtlasTileEdgeWeight(vec2 localUV) {
    vec2 edge = min(localUV, 1.0 - localUV);
    vec2 halfTexel = AtlasTileHalfTexel();
    vec2 fade = smoothstep(halfTexel, halfTexel * 6.0, edge);
    return fade.x * fade.y;
}

vec2 ClampAtlasLocalUV(vec2 localUV) {
    vec2 halfTexel = AtlasTileHalfTexel();
    return clamp(localUV, halfTexel, 1.0 - halfTexel);
}

vec3 ProjectWorldToWaterCascade(vec3 worldPos, int cascadeIndex) {
    vec4 clip = u_WaterCascadeVP[cascadeIndex] * vec4(worldPos, 1.0);
    if (abs(clip.w) <= 0.00001)
        return vec3(-1.0);
    return clip.xyz / clip.w * 0.5 + 0.5;
}

float CascadeBlendAlpha(float receiverLinear, int cascadeIndex) {
    if (cascadeIndex >= 3)
        return 0.0;

    float cascadeStart = cascadeIndex == 0 ? u_NearClip : u_WaterCascadeSplits[cascadeIndex - 1];
    float cascadeEnd = u_WaterCascadeSplits[cascadeIndex];
    float cascadeWidth = max(cascadeEnd - cascadeStart, 0.001);
    return smoothstep(cascadeEnd - cascadeWidth * 0.12, cascadeEnd, receiverLinear);
}

vec4 SampleComputeIrradianceCascade(vec3 receiverWorldPos,
                                    int cascadeIndex,
                                    out bool valid) {
    valid = false;
    vec3 cascadeCoord = ProjectWorldToWaterCascade(receiverWorldPos, cascadeIndex);
    if (any(lessThan(cascadeCoord, vec3(0.0))) ||
        any(greaterThan(cascadeCoord, vec3(1.0)))) {
        return vec4(0.0);
    }

    vec2 localUV = cascadeCoord.xy;
    float edgeWeight = AtlasTileEdgeWeight(localUV);
    vec2 atlasUV = LocalToAtlasUV(ClampAtlasLocalUV(localUV), cascadeIndex);
    valid = true;
    return texture(u_HPWaterCausticComputeIrradiance, atlasUV) * edgeWeight;
}

vec4 SampleComputeIrradiance(vec2 screenUV, float waterDepth, float sceneDepth) {
    if (u_HPWaterCausticComputeEnabled != 1)
        return vec4(0.0);

    if (u_HPWaterCausticAtlasEnabled != 1 ||
        u_HPWaterCausticAtlasWidth <= 1.0 ||
        u_HPWaterCausticAtlasHeight <= 1.0) {
        return texture(u_HPWaterCausticComputeIrradiance, screenUV);
    }

    float receiverDepth = sceneDepth < 0.9999 ? sceneDepth : waterDepth;
    float receiverLinear = LinearizeDepth(receiverDepth);
    vec3 receiverWorldPos = ReconstructWorldPosition(screenUV, receiverDepth);
    int cascadeIndex = SelectWaterCascade(receiverLinear);

    bool currentValid = false;
    vec4 current = SampleComputeIrradianceCascade(receiverWorldPos, cascadeIndex, currentValid);
    float alpha = CascadeBlendAlpha(receiverLinear, cascadeIndex);
    if (cascadeIndex < 3 && alpha > 0.0001) {
        bool nextValid = false;
        vec4 next = SampleComputeIrradianceCascade(receiverWorldPos, cascadeIndex + 1, nextValid);
        if (nextValid) {
            current = currentValid ? mix(current, next, alpha) : next * alpha;
            currentValid = true;
        }
    }

    return currentValid ? current : vec4(0.0);
}

float SampleAtlasFocus(vec2 screenUV, vec2 causticUV, vec3 waterNormal, vec3 lightDir, float thickness) {
    if (u_HPWaterCausticAtlasEnabled == 0 ||
        u_HPWaterCausticAtlasWidth <= 1.0 ||
        u_HPWaterCausticAtlasHeight <= 1.0) {
        return 1.0;
    }

    // Transitional HPWater parity step: consume the light-space cascade atlas as
    // the caustic energy source until the original compute/atomic pass exists.
    vec2 cascadeTile = floor(fract(screenUV * 1.37 + waterNormal.xz * 0.19) * 2.0);
    vec2 atlasLocalUV = fract(causticUV * 0.071 + waterNormal.xz * 0.083 + lightDir.xz * 0.037);
    vec2 atlasUV = (cascadeTile + atlasLocalUV) * 0.5;
    vec2 texel = 1.0 / vec2(u_HPWaterCausticAtlasWidth, u_HPWaterCausticAtlasHeight);

    vec4 atlasCenter = texture(u_HPWaterCausticAtlas, atlasUV);
    vec4 atlasX = texture(u_HPWaterCausticAtlas, atlasUV + vec2(texel.x, 0.0));
    vec4 atlasY = texture(u_HPWaterCausticAtlas, atlasUV + vec2(0.0, texel.y));
    float atlasDepth = texture(u_HPWaterCausticAtlasDepth, atlasUV).r;

    vec3 atlasNormal = normalize(atlasCenter.rgb * 2.0 - 1.0);
    float normalGradient = length(atlasCenter.rgb - atlasX.rgb) + length(atlasCenter.rgb - atlasY.rgb);
    float atlasCoverage = step(0.0001, atlasCenter.a) * step(atlasDepth, 0.9999);
    float slopeFocus = smoothstep(0.015, 0.42, length(atlasNormal.xz));
    float gradientFocus = smoothstep(0.006, 0.19, normalGradient);
    float lightFocus = clamp(dot(atlasNormal, lightDir) * 0.5 + 0.5, 0.0, 1.0);
    float strandFocus = CausticStrands(atlasLocalUV * 18.0 + atlasNormal.xz * 1.7);
    float thicknessFocus = mix(1.16, 0.72, clamp(thickness / 18.0, 0.0, 1.0));

    float atlasFocus = (0.45 + strandFocus * 1.65) *
        (0.35 + slopeFocus * 0.45 + gradientFocus * 0.65) *
        (0.45 + lightFocus * 0.75) *
        thicknessFocus;
    return mix(1.0, atlasFocus, atlasCoverage);
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
    float receiverThicknessMask = sceneDepth < 0.9999
        ? smoothstep(0.015, 0.55, thickness)
        : 0.25;

    vec2 refractedOffset = N.xz * (0.65 + max(thickness, 0.0) * 0.02);
    vec2 lightDrift = normalize(L.xz + vec2(0.001)) * 0.17;
    vec2 causticUV = v_UV * max(u_CausticScale, 0.1) + refractedOffset + lightDrift;

    float slopeFocus = smoothstep(0.025, 0.45, length(N.xz));
    float foamOcclusion = 1.0 - clamp(absorptionFoam.a, 0.0, 1.0) * 0.65;
    float absorptionLum = dot(max(absorptionFoam.rgb, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722));
    float scatterLum = dot(max(scatterThickness.rgb, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722)) * 0.1;
    float transmissionGate = exp(-absorptionLum * max(thickness, 0.0) *
        clamp(u_CausticTransmittanceStrength, 0.0, 8.0) * 0.18);
    float leakGate = mix(1.0, receiverThicknessMask, clamp(u_CausticLeakReduction, 0.0, 1.0));
    float scatterBoost = 1.0 + scatterLum * clamp(u_CausticScatterBoost, 0.0, 4.0);
    float sharedEnergy = slopeFocus * sunFacing * depthFade * foamOcclusion *
        transmissionGate * leakGate * scatterBoost;
    sharedEnergy *= clamp(u_CausticStrength, 0.0, 8.0) * max(u_LightIntensity, 0.0);
    sharedEnergy *= SampleAtlasFocus(v_UV, causticUV, N, L, thickness);

    vec4 computeIrradiance = SampleComputeIrradiance(v_UV, waterDepth, sceneDepth);
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
    float computeWeight = step(0.00001, computeIrradiance.a);
    energyRGB = mix(energyRGB, max(energyRGB * 0.25, computeIrradiance.rgb), 0.82 * computeWeight);

    vec3 absorption = max(absorptionFoam.rgb, vec3(0.0001));
    vec3 waterTint = mix(vec3(1.0), scatterThickness.rgb, 0.24);
    vec3 attenuation = exp(-absorption * (0.45 + thickness * 0.12 *
        (1.0 + clamp(u_CausticTransmittanceStrength, 0.0, 8.0) * 0.35)));
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
