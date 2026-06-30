// VibeEngine ShaderLab - HPWater full-resolution SSR motion vectors.
// Stores currentUV - previousUV so temporal SSR can sample history with:
// historyUV = currentUV - motionVector.

Shader "VibeEngine/HPWaterSSRMotionVector" {
    Properties {
    }

    SubShader {
        Tags { "RenderType"="Opaque" "Queue"="Overlay" "LightMode"="HPWaterSSRMotionVector" }

        Pass {
            Name "HPWaterSSRMotionVectorPass"

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
layout(location = 0) out vec2 MotionVector;

uniform sampler2D u_HPWaterDepth;
uniform mat4 u_CurrentViewProjection;
uniform mat4 u_PreviousViewProjection;
uniform mat4 u_InverseViewProjection;

vec3 ReconstructWorldPosition(vec2 uv, float depth) {
    vec2 ndcXY = uv * 2.0 - 1.0;
    float ndcZ = depth * 2.0 - 1.0;
    vec4 world = u_InverseViewProjection * vec4(ndcXY, ndcZ, 1.0);
    float invW = abs(world.w) > 0.00001 ? 1.0 / world.w : 0.0;
    return world.xyz * invW;
}

vec2 ProjectUV(mat4 viewProjection, vec3 worldPos, out bool valid) {
    vec4 clip = viewProjection * vec4(worldPos, 1.0);
    if (abs(clip.w) <= 0.00001) {
        valid = false;
        return v_UV;
    }

    vec3 ndc = clip.xyz / clip.w;
    vec2 projectedUV = ndc.xy * 0.5 + 0.5;
    valid = ndc.z >= -1.0 && ndc.z <= 1.0 &&
        all(greaterThanEqual(projectedUV, vec2(0.0))) &&
        all(lessThanEqual(projectedUV, vec2(1.0)));
    return projectedUV;
}

void main() {
    float waterDepth = texture(u_HPWaterDepth, v_UV).r;
    if (waterDepth >= 0.9999) {
        MotionVector = vec2(0.0);
        return;
    }

    vec3 waterWorldPos = ReconstructWorldPosition(v_UV, waterDepth);
    bool currentValid = false;
    bool previousValid = false;
    vec2 currentUV = ProjectUV(u_CurrentViewProjection, waterWorldPos, currentValid);
    vec2 previousUV = ProjectUV(u_PreviousViewProjection, waterWorldPos, previousValid);
    if (!currentValid || !previousValid) {
        MotionVector = vec2(0.0);
        return;
    }

    MotionVector = currentUV - previousUV;
}
#endif

            ENDGLSL
        }
    }

    FallBack "VibeEngine/HPWaterSSR"
}
