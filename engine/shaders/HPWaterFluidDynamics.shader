// VibeEngine ShaderLab - HPWater FluidDynamics wave equation pass.
// This mirrors HPWater's ping-pong wave-height texture dataflow using the
// current fullscreen framebuffer path until the engine has a compute backend.

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

uniform float u_WaveSpeed;
uniform float u_DampingFactor;
uniform vec3  u_WaveSourceUV;
uniform float u_WaveSourceIntensity;
uniform float u_WaveSourceRadius;
uniform int   u_ObstacleMaskEnabled;

float ReadHeight(ivec2 coord, ivec2 size) {
    coord = clamp(coord, ivec2(0), size - ivec2(1));
    return texelFetch(u_WaveHeightCurrent, coord, 0).r;
}

float ReadObstacle(ivec2 coord, ivec2 size) {
    if (u_ObstacleMaskEnabled == 0)
        return 0.0;
    coord = clamp(coord, ivec2(0), size - ivec2(1));
    return texelFetch(u_ObstacleMask, coord, 0).r;
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
    float c2 = clamp(u_WaveSpeed * u_WaveSpeed, 0.0, 4.0);
    float damping = clamp(u_DampingFactor, 0.0, 0.98);

    float distToEdgeX = min(float(coord.x), float(size.x - 1 - coord.x));
    float distToEdgeY = min(float(coord.y), float(size.y - 1 - coord.y));
    float edgeWidth = max(float(max(size.x, size.y)) * 0.10, 1.0);
    float edgeRetention = smoothstep(0.0, edgeWidth, min(distToEdgeX, distToEdgeY));
    edgeRetention *= edgeRetention;

    float obstacleContact = max(max(leftObstacle, rightObstacle), max(downObstacle, upObstacle));
    float obstacleRetention = mix(1.0, 0.55, smoothstep(0.01, 1.0, obstacleContact));

    float inertia = (current - previous) * (1.0 - damping) * edgeRetention * obstacleRetention;
    float next = current + inertia + c2 * (1.0 / 3600.0) * laplacian * obstacleRetention;

    if (u_WaveSourceIntensity != 0.0 && u_WaveSourceUV.x >= 0.0 && u_WaveSourceUV.y >= 0.0) {
        vec2 pixelUV = (vec2(coord) + vec2(0.5)) / vec2(size);
        vec2 distPixels = (pixelUV - u_WaveSourceUV.xy) * vec2(size);
        float radius = max(u_WaveSourceRadius, 1.0);
        float r = length(distPixels);
        if (r < radius) {
            float sigma = radius * 0.4;
            float gaussian = exp(-(r * r) / max(2.0 * sigma * sigma, 0.0001));
            next += clamp(gaussian * u_WaveSourceIntensity, -0.08, 0.08) * edgeRetention;
        }
    }

    FragHeight = clamp(next, -1.0, 1.0);
}
#endif

            ENDGLSL
        }
    }

    FallBack "VibeEngine/Unlit"
}
