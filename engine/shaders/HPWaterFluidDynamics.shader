// VibeEngine ShaderLab - HPWater FluidDynamics wave equation pass.
// Kept as a fallback for platforms where the compute backend is unavailable.
// The primary OpenGL path is HPWaterFluidDynamics.comp.

Shader "VibeEngine/HPWaterFluidDynamics" {
    Properties {
    }

    SubShader {
        Tags { "RenderType"="Opaque" "Queue"="Overlay" "LightMode"="HPWaterFluidDynamics" }

        Pass {
            Name "HPWaterFluidDynamicsPass"

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
layout(location = 0) out float FragHeight;

uniform sampler2D u_WaveHeightCurrent;
uniform sampler2D u_WaveHeightPrevious;
uniform sampler2D u_ObstacleMask;
uniform sampler2D u_WaterHeightTexture;
uniform sampler2D u_SceneHeightTexture;

uniform float u_WaveSpeed;
uniform float u_DampingFactor;
uniform float u_DeltaTime;
uniform int   u_WaveSourceCount;
uniform vec3  u_WaveSourceUVRadius[8];
uniform float u_WaveSourceIntensity[8];
uniform int   u_ObstacleMaskEnabled;
uniform int   u_HeightFieldEnabled;
uniform float u_HeightObstacleEpsilon;

float ReadHeight(ivec2 coord, ivec2 size) {
    coord = clamp(coord, ivec2(0), size - ivec2(1));
    return texelFetch(u_WaveHeightCurrent, coord, 0).r;
}

float ReadObstacle(ivec2 coord, ivec2 size) {
    coord = clamp(coord, ivec2(0), size - ivec2(1));
    if (u_HeightFieldEnabled != 0) {
        float waterHeight = texelFetch(u_WaterHeightTexture, coord, 0).r;
        float sceneHeight = texelFetch(u_SceneHeightTexture, coord, 0).r;
        return sceneHeight > waterHeight + max(u_HeightObstacleEpsilon, 0.0) ? 1.0 : 0.0;
    }
    if (u_ObstacleMaskEnabled == 0)
        return 0.0;
    return texelFetch(u_ObstacleMask, coord, 0).r;
}

float ReadSourceImpulse(ivec2 coord, float width) {
    float impulse = 0.0;
    vec2 pixelUV = (vec2(coord) + vec2(0.5)) / width;
    int sourceCount = clamp(u_WaveSourceCount, 0, 8);
    for (int i = 0; i < sourceCount; ++i) {
        float intensity = u_WaveSourceIntensity[i];
        vec3 source = u_WaveSourceUVRadius[i];
        if (intensity <= 0.001 || source.x < 0.0 || source.y < 0.0)
            continue;

        float radius = max(source.z, 0.001);
        vec2 distPixels = (pixelUV - source.xy) * width;
        float r = length(distPixels);
        if (r >= radius)
            continue;

        float sigma = radius * 0.4;
        float gaussian = exp(-(r * r) / (2.0 * sigma * sigma));
        impulse += min(gaussian * intensity, 0.05);
    }
    return impulse;
}

void main() {
    ivec2 size = textureSize(u_WaveHeightCurrent, 0);
    ivec2 coord = clamp(ivec2(v_UV * vec2(size)), ivec2(0), size - ivec2(1));

    float current = texelFetch(u_WaveHeightCurrent, coord, 0).r;
    float previous = texelFetch(u_WaveHeightPrevious, coord, 0).r;
    float obstacle = ReadObstacle(coord, size);

    if (obstacle > 0.5) {
        FragHeight = 0.0;
        return;
    }

    float leftObstacle = ReadObstacle(coord + ivec2(-1, 0), size);
    float rightObstacle = ReadObstacle(coord + ivec2(1, 0), size);
    float downObstacle = ReadObstacle(coord + ivec2(0, -1), size);
    float upObstacle = ReadObstacle(coord + ivec2(0, 1), size);

    float left = mix(ReadHeight(coord + ivec2(-1, 0), size), current, step(0.5, leftObstacle));
    float right = mix(ReadHeight(coord + ivec2(1, 0), size), current, step(0.5, rightObstacle));
    float down = mix(ReadHeight(coord + ivec2(0, -1), size), current, step(0.5, downObstacle));
    float up = mix(ReadHeight(coord + ivec2(0, 1), size), current, step(0.5, upObstacle));

    float laplacian = left + right + down + up - 4.0 * current;
    float c2 = u_WaveSpeed * u_WaveSpeed;
    float damping = u_DampingFactor;
    float dt = clamp(u_DeltaTime, 1.0 / 240.0, 1.0 / 15.0);

    float distToEdgeX = min(float(coord.x), float(size.x - 1 - coord.x));
    float distToEdgeY = min(float(coord.y), float(size.y - 1 - coord.y));
    float width = float(size.x);
    float edgeWidth = max(width * 0.10, 1.0);
    float edgeX = 1.0 - smoothstep(0.0, edgeWidth, distToEdgeX);
    float edgeY = 1.0 - smoothstep(0.0, edgeWidth, distToEdgeY);
    float edgeAbsorption = 1.0 - (1.0 - edgeX) * (1.0 - edgeY);
    edgeAbsorption = clamp(1.0 - edgeAbsorption * edgeAbsorption, 0.0, 1.0);

    float dampingStep = clamp(dt * 60.0, 0.0, 4.0);
    float energyRetention = pow(max(1.0 - damping, 0.0), dampingStep) * edgeAbsorption;
    float inertia = (current - previous) * energyRetention;
    float next = current + inertia + c2 * dt * dt * laplacian;

    next += ReadSourceImpulse(coord, width) * edgeAbsorption;

    FragHeight = next;
}
#endif

            ENDGLSL
        }
    }

    FallBack "VibeEngine/Unlit"
}
