// VibeEngine ShaderLab - HPWater low-resolution volume motion vectors.
// Generates an explicit velocity texture for the volume temporal filter. The
// stored vector follows HPWater's sampled motion-vector convention:
// historyUV = currentUV - motionVector.

Shader "VibeEngine/HPWaterVolumeMotionVector" {
    Properties {
    }

    SubShader {
        Tags { "RenderType"="Opaque" "Queue"="Overlay" "LightMode"="HPWaterVolumeMotionVector" }

        Pass {
            Name "HPWaterVolumeMotionVectorPass"

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

uniform sampler2D u_CurrentDepth;
uniform sampler2D u_HPWaterRefractionWorldData;
uniform sampler2D u_ScenePositionMetallic;
uniform mat4 u_CurrentViewProjection;
uniform mat4 u_PreviousViewProjection;
uniform int u_SceneMotionVectorEnabled;
uniform int u_ObjectMotionVectorEnabled;
uniform vec3 u_ObjectMotionWorldOffset;
uniform int u_ObjectMotionFieldCount;
uniform vec4 u_ObjectMotionSpheres[8];
uniform vec4 u_ObjectMotionOffsets[8];

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

bool IsFinitePosition(vec3 value) {
    return all(equal(value, value)) &&
        all(lessThan(abs(value), vec3(1000000.0))) &&
        dot(value, value) > 0.000001;
}

bool BuildMotionVectorFromWorldPair(vec3 currentWorldPos, vec3 previousWorldPos, out vec2 motionVector) {
    bool currentValid = false;
    bool previousValid = false;
    vec2 currentUV = ProjectUV(u_CurrentViewProjection, currentWorldPos, currentValid);
    vec2 previousUV = ProjectUV(u_PreviousViewProjection, previousWorldPos, previousValid);
    if (!currentValid || !previousValid) {
        motionVector = vec2(0.0);
        return false;
    }

    motionVector = currentUV - previousUV;
    return true;
}

bool BuildMotionVector(vec3 worldPos, out vec2 motionVector) {
    return BuildMotionVectorFromWorldPair(worldPos, worldPos, motionVector);
}

vec3 SelectObjectMotionOffset(vec3 sceneWorld) {
    vec3 selectedOffset = u_ObjectMotionWorldOffset;
    float bestDistanceRatio = 1.0e20;

    for (int i = 0; i < 8; ++i) {
        if (i >= u_ObjectMotionFieldCount) {
            break;
        }

        vec4 sphere = u_ObjectMotionSpheres[i];
        vec3 offset = u_ObjectMotionOffsets[i].xyz;
        float radius = max(sphere.w, 0.0001);
        float dist = length(sceneWorld - sphere.xyz);
        float distanceRatio = dist / radius;
        if (distanceRatio <= 1.0 && distanceRatio < bestDistanceRatio) {
            bestDistanceRatio = distanceRatio;
            selectedOffset = offset;
        }
    }

    return selectedOffset;
}

void main() {
    vec4 currentDepth = texture(u_CurrentDepth, v_UV);
    if (currentDepth.a <= 0.0001) {
        MotionVector = vec2(0.0);
        return;
    }

    if (u_SceneMotionVectorEnabled == 1) {
        vec3 sceneWorld = texture(u_ScenePositionMetallic, v_UV).xyz;
        vec3 objectOffset = u_ObjectMotionVectorEnabled == 1
            ? SelectObjectMotionOffset(sceneWorld)
            : vec3(0.0);
        vec3 previousSceneWorld = u_ObjectMotionVectorEnabled == 1
            ? sceneWorld - objectOffset
            : sceneWorld;
        if (IsFinitePosition(sceneWorld) &&
            IsFinitePosition(previousSceneWorld) &&
            BuildMotionVectorFromWorldPair(sceneWorld, previousSceneWorld, MotionVector)) {
            return;
        }
    }

    vec3 refractWorld = texture(u_HPWaterRefractionWorldData, v_UV).xyz;
    if (!IsFinitePosition(refractWorld) || !BuildMotionVector(refractWorld, MotionVector)) {
        MotionVector = vec2(0.0);
    }
}
#endif

            ENDGLSL
        }
    }

    FallBack "VibeEngine/HPWaterVolumeTemporal"
}
