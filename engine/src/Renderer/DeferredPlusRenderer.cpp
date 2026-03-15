/*
 * DeferredPlusRenderer — Tiled Deferred Rendering implementation.
 *
 * Contains:
 *   - G-Buffer creation (4 MRT + depth)
 *   - Light data SSBO management
 *   - Compute shader for tile-based light culling
 *   - Tiled deferred lighting fullscreen pass
 *
 * All shaders are embedded as inline string constants.
 */
#include "VibeEngine/Renderer/DeferredPlusRenderer.h"
#include "VibeEngine/Renderer/ShaderSources.h"
#include "VibeEngine/Renderer/GPUResourceTracker.h"
#include "VibeEngine/Renderer/ShadowMap.h"
#include "VibeEngine/Scene/Scene.h"
#include "VibeEngine/Scene/Entity.h"
#include "VibeEngine/Scene/Components.h"
#include "VibeEngine/Core/Log.h"

#include <glad/gl.h>
#include <glm/gtc/type_ptr.hpp>
#include <cmath>
#include <algorithm>

namespace VE {

// ═══════════════════════════════════════════════════════════════════════
// ── G-Buffer vertex shader (writes to 4 MRT) ─────────────────────────
// ═══════════════════════════════════════════════════════════════════════

static const char* s_GBufferVertSrc = R"(
#version 460 core
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec3 a_Color;
layout(location = 3) in vec2 a_TexCoord;

uniform mat4 u_MVP;
uniform mat4 u_Model;

out vec3 v_FragPos;
out vec3 v_Normal;
out vec3 v_Color;
out vec2 v_TexCoord;

void main() {
    vec4 worldPos = u_Model * vec4(a_Position, 1.0);
    v_FragPos  = worldPos.xyz;
    v_Normal   = normalize(mat3(transpose(inverse(u_Model))) * a_Normal);
    v_Color    = a_Color;
    v_TexCoord = a_TexCoord;
    gl_Position = u_MVP * vec4(a_Position, 1.0);
}
)";

// ═══════════════════════════════════════════════════════════════════════
// ── G-Buffer fragment shader — writes 4 MRT ──────────────────────────
// ═══════════════════════════════════════════════════════════════════════

static const char* s_GBufferFragSrc = R"(
#version 460 core
in vec3 v_FragPos;
in vec3 v_Normal;
in vec3 v_Color;
in vec2 v_TexCoord;

layout(location = 0) out vec4 gPositionMetallic;  // xyz = world pos, w = metallic
layout(location = 1) out vec4 gNormalRoughness;    // xyz = normal,    w = roughness
layout(location = 2) out vec4 gAlbedoAO;           // rgb = albedo,    a = AO
layout(location = 3) out vec4 gEmissionFlags;      // rgb = emission,  a = flags

// Material properties
uniform vec4  u_EntityColor;
uniform float u_Metallic;
uniform float u_Roughness;
uniform float u_AO;
uniform vec4  u_EmissionColor;
uniform float u_Cutoff;

// Textures
uniform sampler2D u_MainTex;
uniform int u_HasMainTex;
uniform sampler2D u_Texture;
uniform int u_UseTexture;
uniform sampler2D u_MetallicGlossMap;
uniform int u_HasMetallicGlossMap;
uniform sampler2D u_OcclusionMap;
uniform int u_HasOcclusionMap;
uniform sampler2D u_EmissionMap;
uniform int u_HasEmissionMap;

void main() {
    // Albedo
    vec4 baseColor = u_EntityColor;
    baseColor.rgb *= v_Color;
    if (u_HasMainTex == 1)
        baseColor *= texture(u_MainTex, v_TexCoord);
    else if (u_UseTexture == 1)
        baseColor *= texture(u_Texture, v_TexCoord);

    // Alpha clip
    if (u_Cutoff > 0.0 && baseColor.a < u_Cutoff)
        discard;

    // Metallic
    float metallic = u_Metallic;
    if (u_HasMetallicGlossMap == 1)
        metallic *= texture(u_MetallicGlossMap, v_TexCoord).r;

    // AO
    float ao = u_AO;
    if (u_HasOcclusionMap == 1)
        ao *= texture(u_OcclusionMap, v_TexCoord).r;

    // Emission
    vec3 emission = u_EmissionColor.rgb;
    if (u_HasEmissionMap == 1)
        emission *= texture(u_EmissionMap, v_TexCoord).rgb;

    // Write G-Buffer
    gPositionMetallic = vec4(v_FragPos, metallic);
    gNormalRoughness  = vec4(normalize(v_Normal), max(u_Roughness, 0.04));
    gAlbedoAO         = vec4(baseColor.rgb, ao);
    gEmissionFlags    = vec4(emission, 1.0); // flags.a = 1 means "lit fragment"
}
)";

// ═══════════════════════════════════════════════════════════════════════
// ── Tile-based light culling compute shader ───────────────────────────
// ═══════════════════════════════════════════════════════════════════════

static const char* s_TileCullComputeSrc = R"(
#version 460 core
layout(local_size_x = 16, local_size_y = 16) in;

// ── Depth buffer input ──
uniform sampler2D u_DepthMap;
uniform mat4  u_Projection;
uniform mat4  u_View;
uniform uvec2 u_ScreenSize;
uniform uint  u_NumPointLights;
uniform uint  u_NumSpotLights;

// ── Light SSBOs (std430) ──
struct GPUPointLight {
    vec4  positionAndRange;    // xyz=pos, w=range
    vec4  colorAndIntensity;   // rgb=col, a=intensity
    int   shadowIndex;
    float pad0, pad1, pad2;
};

struct GPUSpotLight {
    vec4  posAndRange;         // xyz=pos, w=range
    vec4  dirAndOuterCos;      // xyz=dir, w=cos(outer)
    vec4  colorAndIntensity;   // rgb=col, a=intensity
    float innerCos;
    int   shadowIndex;
    float pad0, pad1;
};

layout(std430, binding = 0) readonly buffer PointLightBuffer {
    GPUPointLight pointLights[];
};

layout(std430, binding = 1) readonly buffer SpotLightBuffer {
    GPUSpotLight spotLights[];
};

// Per-tile output: tileInfo[tileIndex] = (offset into lightIndex buffer, pointCount, spotCount, unused)
layout(std430, binding = 3) writeonly buffer TileInfoBuffer {
    uvec4 tileInfo[];
};

// Flat light index list: [pointIdx0, pointIdx1, ..., spotIdx0, spotIdx1, ...]
layout(std430, binding = 2) writeonly buffer LightIndexBuffer {
    uint lightIndices[];
};

// ── Shared memory for depth min/max reduction ──
shared uint s_MinDepthInt;
shared uint s_MaxDepthInt;

// Per-tile light lists in shared memory
const uint MAX_POINT_PER_TILE = 128;
const uint MAX_SPOT_PER_TILE  = 128;
shared uint s_PointLightCount;
shared uint s_SpotLightCount;
shared uint s_PointLightIndices[MAX_POINT_PER_TILE];
shared uint s_SpotLightIndices[MAX_SPOT_PER_TILE];

// ── Frustum planes (4 side + near + far) ──
shared vec4 s_FrustumPlanes[6];

void main() {
    uvec2 tileID = gl_WorkGroupID.xy;
    uint  localIdx = gl_LocalInvocationIndex;
    uvec2 tileCountXY = uvec2(
        (u_ScreenSize.x + 15u) / 16u,
        (u_ScreenSize.y + 15u) / 16u
    );
    uint tileIndex = tileID.y * tileCountXY.x + tileID.x;

    // ── Step 1: Initialize shared memory ──
    if (localIdx == 0) {
        s_MinDepthInt = 0xFFFFFFFFu;
        s_MaxDepthInt = 0u;
        s_PointLightCount = 0u;
        s_SpotLightCount  = 0u;
    }
    barrier();

    // ── Step 2: Each thread reads one depth value and contributes to min/max ──
    uvec2 pixelCoord = tileID * 16u + gl_LocalInvocationID.xy;
    float depth = 1.0;
    if (pixelCoord.x < u_ScreenSize.x && pixelCoord.y < u_ScreenSize.y) {
        depth = texelFetch(u_DepthMap, ivec2(pixelCoord), 0).r;
    }
    uint depthInt = floatBitsToUint(depth);
    atomicMin(s_MinDepthInt, depthInt);
    atomicMax(s_MaxDepthInt, depthInt);
    barrier();

    // ── Step 3: Build tile frustum (thread 0 only) ──
    if (localIdx == 0) {
        float minDepth = uintBitsToFloat(s_MinDepthInt);
        float maxDepth = uintBitsToFloat(s_MaxDepthInt);

        // Convert tile corners to NDC
        vec2 tileMin = vec2(tileID) * vec2(16.0) / vec2(u_ScreenSize);
        vec2 tileMax = vec2(tileID + 1u) * vec2(16.0) / vec2(u_ScreenSize);
        // Map [0,1] -> [-1,1]
        tileMin = tileMin * 2.0 - 1.0;
        tileMax = tileMax * 2.0 - 1.0;

        // Linearize depth: reconstruct view-space Z from NDC depth
        // For standard perspective projection: z_ndc = (A*z_view + B) / (-z_view)
        // where A = proj[2][2], B = proj[3][2]
        // => z_view = B / (z_ndc + A)  (where z_ndc = depth*2-1 for OpenGL)

        // Build 4 side frustum planes in view space
        // We use the inverse projection to get view-space corners, then derive planes.
        mat4 invProj = inverse(u_Projection);

        // 4 corners of the tile at near plane (z_ndc = -1 in OpenGL)
        vec4 corners[4];
        corners[0] = invProj * vec4(tileMin.x, tileMin.y, -1.0, 1.0);
        corners[1] = invProj * vec4(tileMax.x, tileMin.y, -1.0, 1.0);
        corners[2] = invProj * vec4(tileMax.x, tileMax.y, -1.0, 1.0);
        corners[3] = invProj * vec4(tileMin.x, tileMax.y, -1.0, 1.0);
        for (int i = 0; i < 4; i++) corners[i] /= corners[i].w;

        // Side planes: each plane goes through the origin and two adjacent corners
        // Normal = cross(corner[i], corner[(i+1)%4]), plane D = 0
        // Left
        vec3 n0 = cross(corners[0].xyz, corners[3].xyz);
        s_FrustumPlanes[0] = vec4(normalize(n0), 0.0);
        // Right
        vec3 n1 = cross(corners[2].xyz, corners[1].xyz);
        s_FrustumPlanes[1] = vec4(normalize(n1), 0.0);
        // Bottom
        vec3 n2 = cross(corners[1].xyz, corners[0].xyz);
        s_FrustumPlanes[2] = vec4(normalize(n2), 0.0);
        // Top
        vec3 n3 = cross(corners[3].xyz, corners[2].xyz);
        s_FrustumPlanes[3] = vec4(normalize(n3), 0.0);

        // Near and far planes from depth
        float nearZ_ndc = minDepth * 2.0 - 1.0;
        float farZ_ndc  = maxDepth * 2.0 - 1.0;
        vec4 nearPt = invProj * vec4(0.0, 0.0, nearZ_ndc, 1.0);
        vec4 farPt  = invProj * vec4(0.0, 0.0, farZ_ndc, 1.0);
        float nearZ = nearPt.z / nearPt.w;  // negative in view space
        float farZ  = farPt.z / farPt.w;

        // Near plane: normal = (0,0,-1), d = -nearZ  (points inward: away from camera)
        s_FrustumPlanes[4] = vec4(0.0, 0.0, -1.0, nearZ);
        // Far plane: normal = (0,0,1), d = -farZ
        s_FrustumPlanes[5] = vec4(0.0, 0.0,  1.0, -farZ);
    }
    barrier();

    // ── Step 4: Each thread tests a subset of lights against the frustum ──
    uint threadCount = 16u * 16u; // 256

    // -- Point lights --
    for (uint i = localIdx; i < u_NumPointLights; i += threadCount) {
        vec4 pr = pointLights[i].positionAndRange;
        // Transform light position to view space
        vec3 viewPos = (u_View * vec4(pr.xyz, 1.0)).xyz;
        float radius = pr.w;

        // Sphere-frustum test: check against all 6 planes
        bool visible = true;
        for (int p = 0; p < 6; p++) {
            float dist = dot(s_FrustumPlanes[p].xyz, viewPos) + s_FrustumPlanes[p].w;
            if (dist < -radius) {
                visible = false;
                break;
            }
        }

        if (visible) {
            uint idx = atomicAdd(s_PointLightCount, 1u);
            if (idx < MAX_POINT_PER_TILE)
                s_PointLightIndices[idx] = i;
        }
    }

    // -- Spot lights (simplified: treated as bounding sphere) --
    for (uint i = localIdx; i < u_NumSpotLights; i += threadCount) {
        vec4 pr = spotLights[i].posAndRange;
        vec3 viewPos = (u_View * vec4(pr.xyz, 1.0)).xyz;
        float radius = pr.w;

        bool visible = true;
        for (int p = 0; p < 6; p++) {
            float dist = dot(s_FrustumPlanes[p].xyz, viewPos) + s_FrustumPlanes[p].w;
            if (dist < -radius) {
                visible = false;
                break;
            }
        }

        if (visible) {
            uint idx = atomicAdd(s_SpotLightCount, 1u);
            if (idx < MAX_SPOT_PER_TILE)
                s_SpotLightIndices[idx] = i;
        }
    }
    barrier();

    // ── Step 5: Write results to global buffers (thread 0) ──
    if (localIdx == 0) {
        uint pointCount = min(s_PointLightCount, MAX_POINT_PER_TILE);
        uint spotCount  = min(s_SpotLightCount,  MAX_SPOT_PER_TILE);
        uint totalCount = pointCount + spotCount;

        // Each tile gets MAX_LIGHTS_PER_TILE slots in the flat index buffer
        uint MAX_LIGHTS_PER_TILE = 256u;
        uint offset = tileIndex * MAX_LIGHTS_PER_TILE;

        tileInfo[tileIndex] = uvec4(offset, pointCount, spotCount, 0u);

        // Write point light indices
        for (uint i = 0u; i < pointCount; i++)
            lightIndices[offset + i] = s_PointLightIndices[i];

        // Write spot light indices (after point lights)
        for (uint i = 0u; i < spotCount; i++)
            lightIndices[offset + pointCount + i] = s_SpotLightIndices[i];
    }
}
)";

// ═══════════════════════════════════════════════════════════════════════
// ── Tiled deferred lighting fragment shader ───────────────────────────
// ═══════════════════════════════════════════════════════════════════════

static const char* s_TiledLightingFragSrc = R"(
#version 460 core
layout(location = 0) in vec2 v_UV;
layout(location = 0) out vec4 FragColor;

// ── G-Buffer textures ──
uniform sampler2D u_GPositionMetallic;
uniform sampler2D u_GNormalRoughness;
uniform sampler2D u_GAlbedoAO;
uniform sampler2D u_GEmissionFlags;

// ── Directional light ──
uniform vec3  u_LightDir;
uniform vec3  u_LightColor;
uniform float u_LightIntensity;
uniform vec3  u_ViewPos;

// ── Shadow uniforms (directional CSM) ──
uniform int   u_ShadowEnabled;
uniform sampler2DArrayShadow u_ShadowMap;
uniform mat4  u_LightSpaceMatrices[3];
uniform vec3  u_CascadeSplits;
uniform mat4  u_ViewMatrix;
uniform float u_ShadowBias;
uniform float u_ShadowNormalBias;
uniform int   u_PCFRadius;

// ── Spot light shadow maps (max 2) ──
const int MAX_SPOT_SHADOWS = 2;
uniform int   u_NumSpotShadows;
uniform sampler2DShadow u_SpotShadowMaps[MAX_SPOT_SHADOWS];
uniform mat4  u_SpotLightSpaceMatrices[MAX_SPOT_SHADOWS];

// ── Point light shadow maps (max 2, cube maps) ──
const int MAX_POINT_SHADOWS = 2;
uniform int   u_NumPointShadows;
uniform samplerCube u_PointShadowCubeMaps[MAX_POINT_SHADOWS];
uniform float u_PointShadowFarPlanes[MAX_POINT_SHADOWS];

// ── Tile info ──
uniform uvec2 u_ScreenSize;

// ── Debug ──
uniform int u_DebugOverlay;

// ── Light SSBOs ──
struct GPUPointLight {
    vec4  positionAndRange;
    vec4  colorAndIntensity;
    int   shadowIndex;
    float pad0, pad1, pad2;
};

struct GPUSpotLight {
    vec4  posAndRange;
    vec4  dirAndOuterCos;
    vec4  colorAndIntensity;
    float innerCos;
    int   shadowIndex;
    float pad0, pad1;
};

layout(std430, binding = 0) readonly buffer PointLightBuffer {
    GPUPointLight pointLights[];
};

layout(std430, binding = 1) readonly buffer SpotLightBuffer {
    GPUSpotLight spotLights[];
};

layout(std430, binding = 3) readonly buffer TileInfoBuffer {
    uvec4 tileInfo[];
};

layout(std430, binding = 2) readonly buffer LightIndexBuffer {
    uint lightIndices[];
};

// ── PBR Functions ────────────────────────────────────────────────────

const float PI = 3.14159265359;

float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float NdotH  = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 0.0000001);
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    return GeometrySchlickGGX(max(dot(N, V), 0.0), roughness)
         * GeometrySchlickGGX(max(dot(N, L), 0.0), roughness);
}

vec3 FresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// ── Shadow Functions ────────────────────────────────────────────────

float ShadowCalculation(vec3 fragPos, vec3 normal, vec3 lightDir) {
    if (u_ShadowEnabled == 0)
        return 1.0;

    vec4 fragPosViewSpace = u_ViewMatrix * vec4(fragPos, 1.0);
    float depthValue = -fragPosViewSpace.z;

    int cascade = 2;
    if (depthValue < u_CascadeSplits.x)
        cascade = 0;
    else if (depthValue < u_CascadeSplits.y)
        cascade = 1;

    vec3 biasedPos = fragPos + normal * u_ShadowNormalBias * (1.0 + float(cascade));
    vec4 fragPosLightSpace = u_LightSpaceMatrices[cascade] * vec4(biasedPos, 1.0);
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;

    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0 ||
        projCoords.z > 1.0)
        return 1.0;

    float cosTheta = max(dot(normal, lightDir), 0.0);
    float bias = u_ShadowBias * (1.0 - cosTheta);
    float currentDepth = projCoords.z - bias;

    float shadow = 0.0;
    vec2 texelSize = 1.0 / vec2(textureSize(u_ShadowMap, 0).xy);
    int samples = 0;
    for (int x = -u_PCFRadius; x <= u_PCFRadius; ++x) {
        for (int y = -u_PCFRadius; y <= u_PCFRadius; ++y) {
            vec2 offset = vec2(float(x), float(y)) * texelSize;
            shadow += texture(u_ShadowMap, vec4(projCoords.xy + offset, float(cascade), currentDepth));
            samples++;
        }
    }
    return shadow / float(samples);
}

float SpotShadowCalculation(int shadowIdx, vec3 fragPos, vec3 normal) {
    if (shadowIdx < 0 || shadowIdx >= u_NumSpotShadows)
        return 1.0;

    vec4 fragPosLightSpace = u_SpotLightSpaceMatrices[shadowIdx] * vec4(fragPos, 1.0);
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;

    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0 ||
        projCoords.z > 1.0)
        return 1.0;

    float bias = 0.001;
    float currentDepth = projCoords.z - bias;

    float shadow = 0.0;
    vec2 texelSize;
    if (shadowIdx == 0)
        texelSize = 1.0 / vec2(textureSize(u_SpotShadowMaps[0], 0));
    else
        texelSize = 1.0 / vec2(textureSize(u_SpotShadowMaps[1], 0));

    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            vec3 coord = vec3(projCoords.xy + vec2(float(x), float(y)) * texelSize, currentDepth);
            if (shadowIdx == 0)
                shadow += texture(u_SpotShadowMaps[0], coord);
            else
                shadow += texture(u_SpotShadowMaps[1], coord);
        }
    }
    return shadow / 9.0;
}

float PointShadowCalculation(int shadowIdx, vec3 fragPos, vec3 lightPos) {
    if (shadowIdx < 0 || shadowIdx >= u_NumPointShadows)
        return 1.0;

    vec3 fragToLight = fragPos - lightPos;
    float currentDist = length(fragToLight);
    float farPlane = u_PointShadowFarPlanes[shadowIdx];
    float bias = 0.05;
    float shadow = 0.0;

    float diskRadius = 0.02;
    vec3 sampleOffsets[20] = vec3[](
        vec3( 1,  1,  1), vec3( 1, -1,  1), vec3(-1, -1,  1), vec3(-1,  1,  1),
        vec3( 1,  1, -1), vec3( 1, -1, -1), vec3(-1, -1, -1), vec3(-1,  1, -1),
        vec3( 1,  1,  0), vec3( 1, -1,  0), vec3(-1, -1,  0), vec3(-1,  1,  0),
        vec3( 1,  0,  1), vec3(-1,  0,  1), vec3( 1,  0, -1), vec3(-1,  0, -1),
        vec3( 0,  1,  1), vec3( 0, -1,  1), vec3( 0, -1, -1), vec3( 0,  1, -1)
    );

    for (int s = 0; s < 20; ++s) {
        float closestDepth;
        if (shadowIdx == 0)
            closestDepth = texture(u_PointShadowCubeMaps[0], fragToLight + sampleOffsets[s] * diskRadius).r;
        else
            closestDepth = texture(u_PointShadowCubeMaps[1], fragToLight + sampleOffsets[s] * diskRadius).r;
        closestDepth *= farPlane;
        if (currentDist - bias > closestDepth)
            shadow += 1.0;
    }
    return 1.0 - shadow / 20.0;
}

// ── Main ─────────────────────────────────────────────────────────────

void main() {
    // Sample G-Buffer
    vec4 posMetal  = texture(u_GPositionMetallic, v_UV);
    vec4 normRough = texture(u_GNormalRoughness,  v_UV);
    vec4 albedoAO  = texture(u_GAlbedoAO,        v_UV);
    vec4 emitFlags = texture(u_GEmissionFlags,    v_UV);

    // Skip sky/unlit pixels (flags.a == 0)
    if (emitFlags.a < 0.5) {
        FragColor = vec4(0.0);
        return;
    }

    vec3  fragPos   = posMetal.xyz;
    float metallic  = posMetal.w;
    vec3  N         = normalize(normRough.xyz);
    float roughness = normRough.w;
    vec3  albedo    = albedoAO.rgb;
    float ao        = albedoAO.a;
    vec3  emission  = emitFlags.rgb;

    vec3 V  = normalize(u_ViewPos - fragPos);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    vec3 Lo = vec3(0.0);

    // ── Directional light ──
    {
        vec3 L = normalize(u_LightDir);
        vec3 H = normalize(V + L);
        vec3 radiance = u_LightColor * u_LightIntensity;

        float NDF = DistributionGGX(N, H, roughness);
        float G   = GeometrySmith(N, V, L, roughness);
        vec3  F   = FresnelSchlick(max(dot(H, V), 0.0), F0);

        vec3  spec = (NDF * G * F) / max(4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0), 0.001);
        vec3  kD = (vec3(1.0) - F) * (1.0 - metallic);
        float NdotL = max(dot(N, L), 0.0);

        float shadow = ShadowCalculation(fragPos, N, L);
        Lo += (kD * albedo / PI + spec) * radiance * NdotL * shadow;
    }

    // ── Compute tile index ──
    ivec2 pixelCoord = ivec2(gl_FragCoord.xy);
    uint tileX = uint(pixelCoord.x) / 16u;
    uint tileY = uint(pixelCoord.y) / 16u;
    uint tilesX = (u_ScreenSize.x + 15u) / 16u;
    uint tileIndex = tileY * tilesX + tileX;

    uvec4 info = tileInfo[tileIndex];
    uint offset     = info.x;
    uint pointCount = info.y;
    uint spotCount  = info.z;

    // ── Point lights (from tile list) ──
    for (uint i = 0u; i < pointCount; i++) {
        uint lightIdx = lightIndices[offset + i];
        vec3 lightPos = pointLights[lightIdx].positionAndRange.xyz;
        float range   = pointLights[lightIdx].positionAndRange.w;
        vec3 lightCol = pointLights[lightIdx].colorAndIntensity.rgb;
        float lightInt= pointLights[lightIdx].colorAndIntensity.a;
        int  shadowIdx= pointLights[lightIdx].shadowIndex;

        vec3  lightVec = lightPos - fragPos;
        float dist     = length(lightVec);
        if (dist > range) continue;

        vec3  L = lightVec / dist;
        vec3  H = normalize(V + L);

        float attenuation = 1.0 / (dist * dist + 1.0);
        float window = 1.0 - pow(clamp(dist / range, 0.0, 1.0), 4.0);
        window = window * window;
        attenuation *= window;

        vec3 radiance = lightCol * lightInt * attenuation;

        float NDF = DistributionGGX(N, H, roughness);
        float G   = GeometrySmith(N, V, L, roughness);
        vec3  F   = FresnelSchlick(max(dot(H, V), 0.0), F0);

        vec3  spec = (NDF * G * F) / max(4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0), 0.001);
        vec3  kD = (vec3(1.0) - F) * (1.0 - metallic);
        float NdotL = max(dot(N, L), 0.0);

        float pointShadow = PointShadowCalculation(shadowIdx, fragPos, lightPos);
        Lo += (kD * albedo / PI + spec) * radiance * NdotL * pointShadow;
    }

    // ── Spot lights (from tile list) ──
    for (uint i = 0u; i < spotCount; i++) {
        uint lightIdx = lightIndices[offset + pointCount + i];
        vec3  lightPos = spotLights[lightIdx].posAndRange.xyz;
        float range    = spotLights[lightIdx].posAndRange.w;
        vec3  lightDir = spotLights[lightIdx].dirAndOuterCos.xyz;
        float outerCos = spotLights[lightIdx].dirAndOuterCos.w;
        vec3  lightCol = spotLights[lightIdx].colorAndIntensity.rgb;
        float lightInt = spotLights[lightIdx].colorAndIntensity.a;
        float innerCos = spotLights[lightIdx].innerCos;
        int   shadowIdx= spotLights[lightIdx].shadowIndex;

        vec3  lightVec = lightPos - fragPos;
        float dist     = length(lightVec);
        if (dist > range) continue;

        vec3 L = lightVec / dist;

        // Spot cone
        float theta   = dot(L, normalize(-lightDir));
        float epsilon = innerCos - outerCos;
        float spotAtt = clamp((theta - outerCos) / max(epsilon, 0.001), 0.0, 1.0);
        if (spotAtt <= 0.0) continue;

        vec3 H = normalize(V + L);

        float attenuation = 1.0 / (dist * dist + 1.0);
        float window = 1.0 - pow(clamp(dist / range, 0.0, 1.0), 4.0);
        window = window * window;
        attenuation *= window * spotAtt;

        vec3 radiance = lightCol * lightInt * attenuation;

        float NDF = DistributionGGX(N, H, roughness);
        float G   = GeometrySmith(N, V, L, roughness);
        vec3  F   = FresnelSchlick(max(dot(H, V), 0.0), F0);

        vec3  spec = (NDF * G * F) / max(4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0), 0.001);
        vec3  kD = (vec3(1.0) - F) * (1.0 - metallic);
        float NdotL = max(dot(N, L), 0.0);

        float spotShadow = SpotShadowCalculation(shadowIdx, fragPos, N);
        Lo += (kD * albedo / PI + spec) * radiance * NdotL * spotShadow;
    }

    // ── Ambient + Emission ──
    vec3 ambient = vec3(0.03) * albedo * ao;
    vec3 color = ambient + Lo + emission;

    // ── Debug overlay: color tiles by light count ──
    if (u_DebugOverlay == 1) {
        float totalLights = float(pointCount + spotCount);
        float heat = clamp(totalLights / 32.0, 0.0, 1.0);
        vec3 heatColor = mix(vec3(0.0, 0.0, 0.2), vec3(1.0, 0.0, 0.0), heat);
        color = mix(color, heatColor, 0.4);
    }

    FragColor = vec4(color, 1.0);
}
)";

// ═══════════════════════════════════════════════════════════════════════
// ── Helper: compile a GL shader program ──────────────────────────────
// ═══════════════════════════════════════════════════════════════════════

static GLuint CompileShaderStage(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> log(static_cast<size_t>(len));
        glGetShaderInfoLog(shader, len, &len, log.data());
        VE_ENGINE_ERROR("DeferredPlus shader compile error: {0}", log.data());
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint LinkProgram(GLuint* shaders, int count) {
    GLuint program = glCreateProgram();
    for (int i = 0; i < count; i++)
        glAttachShader(program, shaders[i]);
    glLinkProgram(program);

    GLint ok;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> log(static_cast<size_t>(len));
        glGetProgramInfoLog(program, len, &len, log.data());
        VE_ENGINE_ERROR("DeferredPlus program link error: {0}", log.data());
    }

    for (int i = 0; i < count; i++)
        glDeleteShader(shaders[i]);
    return program;
}

// ═══════════════════════════════════════════════════════════════════════
// ── DeferredPlusRenderer implementation ──────────────────────────────
// ═══════════════════════════════════════════════════════════════════════

DeferredPlusRenderer::~DeferredPlusRenderer() {
    Shutdown();
}

void DeferredPlusRenderer::Init(uint32_t width, uint32_t height) {
    if (m_Initialized) return;
    m_Width  = width;
    m_Height = height;

    CompileShaders();
    CreateGBuffer();
    RecalculateTileGrid();
    CreateSSBOs();

    glGenVertexArrays(1, &m_QuadVAO);
    VE_GPU_TRACK(GPUResourceType::VertexArray, m_QuadVAO);

    m_Initialized = true;
    VE_ENGINE_INFO("DeferredPlusRenderer initialized ({0}x{1}, {2}x{3} tiles)",
                   width, height, m_TileCountX, m_TileCountY);
}

void DeferredPlusRenderer::Shutdown() {
    if (!m_Initialized) return;

    DestroyGBuffer();
    DestroySSBOs();
    DestroyShaders();

    if (m_QuadVAO) {
        VE_GPU_UNTRACK(GPUResourceType::VertexArray, m_QuadVAO);
        glDeleteVertexArrays(1, &m_QuadVAO);
        m_QuadVAO = 0;
    }

    m_Initialized = false;
    VE_ENGINE_INFO("DeferredPlusRenderer shut down");
}

void DeferredPlusRenderer::Resize(uint32_t width, uint32_t height) {
    if (width == m_Width && height == m_Height) return;
    m_Width  = width;
    m_Height = height;

    DestroyGBuffer();
    CreateGBuffer();

    uint32_t oldTotalTiles = m_TotalTiles;
    RecalculateTileGrid();
    if (m_TotalTiles != oldTotalTiles) {
        DestroySSBOs();
        CreateSSBOs();
    }
}

// ── G-Buffer ─────────────────────────────────────────────────────────

void DeferredPlusRenderer::CreateGBuffer() {
    glGenFramebuffers(1, &m_GBufferFBO);
    VE_GPU_TRACK(GPUResourceType::Framebuffer, m_GBufferFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_GBufferFBO);

    // 4 color attachments (RGBA16F for HDR precision)
    GLenum formats[4] = { GL_RGBA16F, GL_RGBA16F, GL_RGBA8, GL_RGBA16F };
    GLenum attachments[4] = {
        GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1,
        GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3
    };

    for (int i = 0; i < 4; i++) {
        glGenTextures(1, &m_GBufferTextures[i]);
        VE_GPU_TRACK(GPUResourceType::Texture, m_GBufferTextures[i]);
        glBindTexture(GL_TEXTURE_2D, m_GBufferTextures[i]);
        GLenum dataFmt = (formats[i] == GL_RGBA8) ? GL_UNSIGNED_BYTE : GL_FLOAT;
        GLenum pixelFmt = GL_RGBA;
        glTexImage2D(GL_TEXTURE_2D, 0, formats[i], m_Width, m_Height, 0,
                     pixelFmt, dataFmt, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, attachments[i], GL_TEXTURE_2D,
                               m_GBufferTextures[i], 0);
    }

    // Depth texture (24-bit depth)
    glGenTextures(1, &m_DepthTexture);
    VE_GPU_TRACK(GPUResourceType::Texture, m_DepthTexture);
    glBindTexture(GL_TEXTURE_2D, m_DepthTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, m_Width, m_Height, 0,
                 GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                           m_DepthTexture, 0);

    // Set draw buffers
    glDrawBuffers(4, attachments);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        VE_ENGINE_ERROR("DeferredPlus: G-Buffer framebuffer is not complete!");

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void DeferredPlusRenderer::DestroyGBuffer() {
    for (int i = 0; i < 4; i++) {
        if (m_GBufferTextures[i]) {
            VE_GPU_UNTRACK(GPUResourceType::Texture, m_GBufferTextures[i]);
            glDeleteTextures(1, &m_GBufferTextures[i]);
            m_GBufferTextures[i] = 0;
        }
    }
    if (m_DepthTexture) {
        VE_GPU_UNTRACK(GPUResourceType::Texture, m_DepthTexture);
        glDeleteTextures(1, &m_DepthTexture);
        m_DepthTexture = 0;
    }
    if (m_GBufferFBO) {
        VE_GPU_UNTRACK(GPUResourceType::Framebuffer, m_GBufferFBO);
        glDeleteFramebuffers(1, &m_GBufferFBO);
        m_GBufferFBO = 0;
    }
}

// ── SSBOs ────────────────────────────────────────────────────────────

void DeferredPlusRenderer::CreateSSBOs() {
    auto createSSBO = [](uint32_t& ssbo, GLsizeiptr size, GLenum usage) {
        glGenBuffers(1, &ssbo);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
        glBufferData(GL_SHADER_STORAGE_BUFFER, size, nullptr, usage);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    };

    // Point/Spot light buffers — initial capacity, resized on upload
    createSSBO(m_PointLightSSBO, sizeof(GPUPointLight) * 16, GL_DYNAMIC_DRAW);
    createSSBO(m_SpotLightSSBO,  sizeof(GPUSpotLight) * 16,  GL_DYNAMIC_DRAW);

    // Per-tile light index buffer
    GLsizeiptr indexBufSize = static_cast<GLsizeiptr>(m_TotalTiles) * MAX_LIGHTS_PER_TILE * sizeof(uint32_t);
    createSSBO(m_LightIndexSSBO, indexBufSize, GL_DYNAMIC_DRAW);

    // Per-tile info buffer (offset, pointCount, spotCount, unused)
    GLsizeiptr tileBufSize = static_cast<GLsizeiptr>(m_TotalTiles) * sizeof(glm::uvec4);
    createSSBO(m_TileInfoSSBO, tileBufSize, GL_DYNAMIC_DRAW);
}

void DeferredPlusRenderer::DestroySSBOs() {
    auto deleteSSBO = [](uint32_t& ssbo) {
        if (ssbo) { glDeleteBuffers(1, &ssbo); ssbo = 0; }
    };
    deleteSSBO(m_PointLightSSBO);
    deleteSSBO(m_SpotLightSSBO);
    deleteSSBO(m_LightIndexSSBO);
    deleteSSBO(m_TileInfoSSBO);
}

// ── Shaders ──────────────────────────────────────────────────────────

void DeferredPlusRenderer::CompileShaders() {
    // Compute shader for tile culling
    {
        GLuint cs = CompileShaderStage(GL_COMPUTE_SHADER, s_TileCullComputeSrc);
        if (cs) {
            m_CullComputeProgram = LinkProgram(&cs, 1);
            VE_GPU_TRACK(GPUResourceType::ShaderProgram, m_CullComputeProgram);
        }
    }

    // Lighting fullscreen pass (vertex from shared source + custom fragment)
    {
        GLuint stages[2];
        stages[0] = CompileShaderStage(GL_VERTEX_SHADER, QuadVertexShaderSrc);
        stages[1] = CompileShaderStage(GL_FRAGMENT_SHADER, s_TiledLightingFragSrc);
        if (stages[0] && stages[1]) {
            m_LightingProgram = LinkProgram(stages, 2);
            VE_GPU_TRACK(GPUResourceType::ShaderProgram, m_LightingProgram);
        }
    }
}

void DeferredPlusRenderer::DestroyShaders() {
    if (m_CullComputeProgram) {
        VE_GPU_UNTRACK(GPUResourceType::ShaderProgram, m_CullComputeProgram);
        glDeleteProgram(m_CullComputeProgram);
        m_CullComputeProgram = 0;
    }
    if (m_LightingProgram) {
        VE_GPU_UNTRACK(GPUResourceType::ShaderProgram, m_LightingProgram);
        glDeleteProgram(m_LightingProgram);
        m_LightingProgram = 0;
    }
}

void DeferredPlusRenderer::RecalculateTileGrid() {
    m_TileCountX = (m_Width  + TILE_SIZE - 1) / TILE_SIZE;
    m_TileCountY = (m_Height + TILE_SIZE - 1) / TILE_SIZE;
    m_TotalTiles = m_TileCountX * m_TileCountY;

    m_Stats.TileCountX = m_TileCountX;
    m_Stats.TileCountY = m_TileCountY;
    m_Stats.TotalTiles = m_TotalTiles;
}

// ── Per-frame pipeline steps ─────────────────────────────────────────

void DeferredPlusRenderer::BeginGBufferPass() {
    glBindFramebuffer(GL_FRAMEBUFFER, m_GBufferFBO);
    glViewport(0, 0, m_Width, m_Height);

    // Clear all attachments
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
}

void DeferredPlusRenderer::EndGBufferPass() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void DeferredPlusRenderer::UploadLights(Scene& scene) {
    m_PointLights.clear();
    m_SpotLights.clear();

    auto& registry = scene.GetRegistry();

    // Gather ALL point lights (no limit!)
    {
        int shadowIdx = 0;
        auto plView = registry.view<TransformComponent, PointLightComponent>();
        for (auto plEntity : plView) {
            if (!scene.IsEntityActiveInHierarchy(plEntity)) continue;
            auto [tc, pl] = plView.get<TransformComponent, PointLightComponent>(plEntity);
            glm::mat4 worldMat = scene.GetWorldTransform(plEntity);

            GPUPointLight gpu;
            gpu.PositionAndRange = glm::vec4(glm::vec3(worldMat[3]), pl.Range);
            gpu.ColorAndIntensity = glm::vec4(pl.Color[0], pl.Color[1], pl.Color[2], pl.Intensity);
            gpu.ShadowIndex = (pl.CastShadows && shadowIdx < 2) ? shadowIdx++ : -1;
            m_PointLights.push_back(gpu);
        }
    }

    // Gather ALL spot lights (no limit!)
    {
        int shadowIdx = 0;
        auto slView = registry.view<TransformComponent, SpotLightComponent>();
        for (auto slEntity : slView) {
            if (!scene.IsEntityActiveInHierarchy(slEntity)) continue;
            auto [tc, sl] = slView.get<TransformComponent, SpotLightComponent>(slEntity);
            glm::mat4 worldMat = scene.GetWorldTransform(slEntity);

            glm::vec3 localDir = glm::normalize(glm::vec3(sl.Direction[0], sl.Direction[1], sl.Direction[2]));
            glm::vec3 worldDir = glm::normalize(glm::mat3(worldMat) * localDir);

            GPUSpotLight gpu;
            gpu.PosAndRange = glm::vec4(glm::vec3(worldMat[3]), sl.Range);
            gpu.DirAndOuterCos = glm::vec4(worldDir, std::cos(glm::radians(sl.OuterAngle)));
            gpu.ColorAndIntensity = glm::vec4(sl.Color[0], sl.Color[1], sl.Color[2], sl.Intensity);
            gpu.InnerCos = std::cos(glm::radians(sl.InnerAngle));
            gpu.ShadowIndex = (sl.CastShadows && shadowIdx < 2) ? shadowIdx++ : -1;
            m_SpotLights.push_back(gpu);
        }
    }

    // Upload to SSBOs (resize if needed)
    auto uploadSSBO = [](uint32_t ssbo, const void* data, size_t size, GLuint binding) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
        // Orphan + upload
        glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(size),
                     data, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, ssbo);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    };

    size_t pointSize = std::max(m_PointLights.size(), size_t(1)) * sizeof(GPUPointLight);
    uploadSSBO(m_PointLightSSBO, m_PointLights.data(), pointSize, 0);

    size_t spotSize = std::max(m_SpotLights.size(), size_t(1)) * sizeof(GPUSpotLight);
    uploadSSBO(m_SpotLightSSBO, m_SpotLights.data(), spotSize, 1);

    m_Stats.NumPointLights = static_cast<uint32_t>(m_PointLights.size());
    m_Stats.NumSpotLights  = static_cast<uint32_t>(m_SpotLights.size());
}

void DeferredPlusRenderer::CullLights(const glm::mat4& projection, const glm::mat4& view) {
    if (!m_CullComputeProgram) return;

    // Bind SSBOs
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_PointLightSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_SpotLightSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_LightIndexSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_TileInfoSSBO);

    glUseProgram(m_CullComputeProgram);

    // Bind depth texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_DepthTexture);
    glUniform1i(glGetUniformLocation(m_CullComputeProgram, "u_DepthMap"), 0);

    // Set uniforms
    glUniformMatrix4fv(glGetUniformLocation(m_CullComputeProgram, "u_Projection"),
                       1, GL_FALSE, glm::value_ptr(projection));
    glUniformMatrix4fv(glGetUniformLocation(m_CullComputeProgram, "u_View"),
                       1, GL_FALSE, glm::value_ptr(view));
    glUniform2ui(glGetUniformLocation(m_CullComputeProgram, "u_ScreenSize"),
                 m_Width, m_Height);
    glUniform1ui(glGetUniformLocation(m_CullComputeProgram, "u_NumPointLights"),
                 static_cast<GLuint>(m_PointLights.size()));
    glUniform1ui(glGetUniformLocation(m_CullComputeProgram, "u_NumSpotLights"),
                 static_cast<GLuint>(m_SpotLights.size()));

    // Dispatch: one workgroup per tile (16x16 threads each)
    glDispatchCompute(m_TileCountX, m_TileCountY, 1);

    // Memory barrier: ensure compute writes are visible to fragment shader
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    glUseProgram(0);
}

void DeferredPlusRenderer::LightingPass(const glm::mat4& view, const glm::mat4& projection,
                                         const glm::vec3& cameraPos, Scene& scene) {
    if (!m_LightingProgram) return;

    // The caller should have bound their HDR framebuffer before calling this.
    // We render a fullscreen quad that reads the G-Buffer + tile data.

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

    glUseProgram(m_LightingProgram);

    // Bind G-Buffer textures
    for (int i = 0; i < 4; i++) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, m_GBufferTextures[i]);
    }
    glUniform1i(glGetUniformLocation(m_LightingProgram, "u_GPositionMetallic"), 0);
    glUniform1i(glGetUniformLocation(m_LightingProgram, "u_GNormalRoughness"),  1);
    glUniform1i(glGetUniformLocation(m_LightingProgram, "u_GAlbedoAO"),        2);
    glUniform1i(glGetUniformLocation(m_LightingProgram, "u_GEmissionFlags"),    3);

    // Bind SSBOs
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_PointLightSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_SpotLightSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_LightIndexSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_TileInfoSSBO);

    // Directional light
    glm::vec3 lightDir = glm::normalize(glm::vec3(0.3f, 1.0f, 0.5f));
    glm::vec3 lightColor(1.0f);
    float lightIntensity = 1.0f;
    {
        auto& registry = scene.GetRegistry();
        auto lightView = registry.view<DirectionalLightComponent>();
        for (auto lightEntity : lightView) {
            if (!scene.IsEntityActiveInHierarchy(lightEntity)) continue;
            auto& dl = lightView.get<DirectionalLightComponent>(lightEntity);
            glm::vec3 dir(dl.Direction[0], dl.Direction[1], dl.Direction[2]);
            float len = glm::length(dir);
            if (len > 0.0001f) lightDir = dir / len;
            lightColor = glm::vec3(dl.Color[0], dl.Color[1], dl.Color[2]);
            lightIntensity = dl.Intensity;
            break;
        }
    }

    glUniform3fv(glGetUniformLocation(m_LightingProgram, "u_LightDir"),
                 1, glm::value_ptr(lightDir));
    glUniform3fv(glGetUniformLocation(m_LightingProgram, "u_LightColor"),
                 1, glm::value_ptr(lightColor));
    glUniform1f(glGetUniformLocation(m_LightingProgram, "u_LightIntensity"), lightIntensity);
    glUniform3fv(glGetUniformLocation(m_LightingProgram, "u_ViewPos"),
                 1, glm::value_ptr(cameraPos));

    glUniform2ui(glGetUniformLocation(m_LightingProgram, "u_ScreenSize"), m_Width, m_Height);
    glUniform1i(glGetUniformLocation(m_LightingProgram, "u_DebugOverlay"),
                m_Stats.DebugOverlay ? 1 : 0);

    // Shadow uniforms
    auto& ps = scene.GetPipelineSettings();
    ShadowMap* shadowMap = scene.GetShadowMap();
    if (shadowMap && ps.ShadowEnabled) {
        glUniform1i(glGetUniformLocation(m_LightingProgram, "u_ShadowEnabled"), 1);
        shadowMap->BindForReading(8);
        glUniform1i(glGetUniformLocation(m_LightingProgram, "u_ShadowMap"), 8);
        glUniform1f(glGetUniformLocation(m_LightingProgram, "u_ShadowBias"), ps.ShadowBias);
        glUniform1f(glGetUniformLocation(m_LightingProgram, "u_ShadowNormalBias"), ps.ShadowNormalBias);
        glUniform1i(glGetUniformLocation(m_LightingProgram, "u_PCFRadius"), ps.ShadowPCFRadius);
        glUniformMatrix4fv(glGetUniformLocation(m_LightingProgram, "u_ViewMatrix"),
                           1, GL_FALSE, glm::value_ptr(view));
        for (int c = 0; c < ShadowMap::NUM_CASCADES; ++c) {
            std::string name = "u_LightSpaceMatrices[" + std::to_string(c) + "]";
            glUniformMatrix4fv(glGetUniformLocation(m_LightingProgram, name.c_str()),
                               1, GL_FALSE, glm::value_ptr(shadowMap->GetLightSpaceMatrix(c)));
        }
        glUniform3f(glGetUniformLocation(m_LightingProgram, "u_CascadeSplits"),
                    shadowMap->GetCascadeSplit(0), shadowMap->GetCascadeSplit(1),
                    shadowMap->GetCascadeSplit(2));
    } else {
        glUniform1i(glGetUniformLocation(m_LightingProgram, "u_ShadowEnabled"), 0);
    }

    // Spot shadow maps
    glUniform1i(glGetUniformLocation(m_LightingProgram, "u_NumSpotShadows"), 0);
    // Point shadow maps
    glUniform1i(glGetUniformLocation(m_LightingProgram, "u_NumPointShadows"), 0);

    // Draw fullscreen triangle
    glBindVertexArray(m_QuadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindVertexArray(0);
    glUseProgram(0);

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
}

} // namespace VE
