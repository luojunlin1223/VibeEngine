// VibeEngine ShaderLab - HPWater composite pass
// Consumes opaque scene color/depth and dedicated HPWater G-buffer data to
// produce the first refraction/composite slice of the HPWater pipeline.

Shader "VibeEngine/HPWaterComposite" {
    Properties {
    }

    SubShader {
        Tags { "RenderType"="Opaque" "Queue"="Overlay" "LightMode"="HPWaterComposite" }

        Pass {
            Name "HPWaterCompositePass"

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
layout(location = 1) out vec4 RefractData;

uniform sampler2D u_SceneColor;
uniform sampler2D u_SceneDepth;
uniform sampler2D u_HPWaterNormalRoughness;
uniform sampler2D u_HPWaterScatterThickness;
uniform sampler2D u_HPWaterAbsorptionFoam;
uniform sampler2D u_HPWaterDepth;

uniform float u_NearClip;
uniform float u_FarClip;
uniform float u_RefractionStrength;

float LinearizeDepth(float depth) {
    float z = depth * 2.0 - 1.0;
    return (2.0 * u_NearClip * u_FarClip) /
        max(u_FarClip + u_NearClip - z * (u_FarClip - u_NearClip), 0.0001);
}

float ScreenEdgeFade(vec2 uv) {
    vec2 edge = min(uv, vec2(1.0) - uv);
    return clamp(min(edge.x, edge.y) * 8.0, 0.0, 1.0);
}

vec2 FindRefractedUV(vec2 uv, vec2 direction, float waterLinearDepth, float sceneLinearDepth, float depthTintDistance) {
    float directionLength = length(direction);
    if (directionLength < 0.00001) {
        return uv;
    }

    vec2 rayDirection = direction / directionLength;
    float maxTravel = clamp(directionLength, 0.0, 0.16) * ScreenEdgeFade(uv);
    if (maxTravel <= 0.00001) {
        return uv;
    }

    const int sampleCount = 16;
    const float expFactor = 8.0;
    float maxCrossDistance = clamp(depthTintDistance, 0.5, 80.0);
    float hitTolerance = max(0.25, maxCrossDistance * 0.025);
    vec2 bestUV = clamp(uv + rayDirection * maxTravel, vec2(0.001), vec2(0.999));
    bool hit = false;

    for (int i = 1; i <= sampleCount; ++i) {
        float linearStep = float(i) / float(sampleCount);
        float d = (pow(expFactor, linearStep) - 1.0) / (expFactor - 1.0);
        vec2 sampleUV = clamp(uv + rayDirection * maxTravel * d, vec2(0.001), vec2(0.999));
        float sampleDepth = texture(u_SceneDepth, sampleUV).r;

        if (sampleDepth >= 0.9999) {
            continue;
        }

        float sampleLinear = LinearizeDepth(sampleDepth);
        float rayLinear = waterLinearDepth + maxCrossDistance * d;

        if (sampleLinear <= waterLinearDepth + 0.02) {
            continue;
        }

        if (rayLinear >= sampleLinear - hitTolerance) {
            bestUV = sampleUV;
            hit = true;
            break;
        }
    }

    if (!hit && sceneLinearDepth <= waterLinearDepth + 0.02) {
        return uv;
    }

    return bestUV;
}

void main() {
    vec4 sceneColor = texture(u_SceneColor, v_UV);
    float waterDepth = texture(u_HPWaterDepth, v_UV).r;

    if (waterDepth >= 0.9999) {
        FragColor = sceneColor;
        RefractData = vec4(v_UV, 1.0, 0.0);
        return;
    }

    float sceneDepth = texture(u_SceneDepth, v_UV).r;

    // Preserve foreground opaque objects. GL depth is smaller when closer.
    if (sceneDepth < waterDepth - 0.00005) {
        FragColor = sceneColor;
        RefractData = vec4(v_UV, sceneDepth, 0.0);
        return;
    }

    vec4 normalRoughness = texture(u_HPWaterNormalRoughness, v_UV);
    vec4 scatterThickness = texture(u_HPWaterScatterThickness, v_UV);
    vec4 absorptionFoam = texture(u_HPWaterAbsorptionFoam, v_UV);

    vec3 N = normalize(normalRoughness.xyz * 2.0 - 1.0);
    float roughness = clamp(normalRoughness.a, 0.02, 0.85);
    vec3 scatterColor = max(scatterThickness.rgb, vec3(0.0));
    float depthTintDistance = max(scatterThickness.a, 0.1);
    vec3 absorptionColor = max(absorptionFoam.rgb, vec3(0.0001));
    float foam = clamp(absorptionFoam.a, 0.0, 1.0);

    float waterLinear = LinearizeDepth(waterDepth);
    float sceneLinear = sceneDepth >= 0.9999
        ? waterLinear + depthTintDistance
        : LinearizeDepth(sceneDepth);
    float thickness = max(sceneLinear - waterLinear, 0.0);
    float normalizedThickness = clamp(thickness / depthTintDistance, 0.0, 1.0);

    // HPWater-style refraction slice: march a normal-driven screen ray until
    // it reaches opaque scene depth. This is the full-resolution counterpart
    // of the reference pass before introducing a depth pyramid.
    vec2 distortion = N.xz * (0.018 + 0.032 * (1.0 - roughness)) *
        clamp(u_RefractionStrength, 0.0, 2.0) *
        (0.25 + 0.75 * normalizedThickness);
    vec2 refractUV = FindRefractedUV(v_UV, distortion, waterLinear, sceneLinear, depthTintDistance);

    float refractedSceneDepth = texture(u_SceneDepth, refractUV).r;
    if (refractedSceneDepth < waterDepth - 0.00005) {
        refractUV = v_UV;
        refractedSceneDepth = sceneDepth;
    }

    vec3 refractedColor = texture(u_SceneColor, refractUV).rgb;
    vec3 transmittance = exp(-absorptionColor * (0.35 + normalizedThickness * 2.35));
    vec3 bodyColor = refractedColor * transmittance +
        scatterColor * (vec3(1.0) - transmittance) * (0.45 + 0.35 * normalizedThickness);

    float fresnel = pow(clamp(1.0 - max(N.y, 0.0), 0.0, 1.0), 2.0);
    vec3 skyTint = mix(scatterColor, vec3(0.78, 0.88, 0.98), 0.55);
    vec3 reflected = skyTint * (0.08 + 0.40 * fresnel);

    vec3 foamColor = mix(vec3(0.88, 0.94, 0.98), vec3(1.0), foam);
    vec3 waterColor = mix(bodyColor + reflected, foamColor, foam * 0.65);
    float waterAlpha = clamp(0.28 + normalizedThickness * 0.62 + foam * 0.45, 0.0, 0.92);

    FragColor = vec4(mix(sceneColor.rgb, waterColor, waterAlpha), sceneColor.a);
    RefractData = vec4(refractUV, refractedSceneDepth, normalizedThickness);
}
#endif

            ENDGLSL
        }
    }

    FallBack "VibeEngine/DeferredLighting"
}
