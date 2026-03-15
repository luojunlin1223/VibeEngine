/*
 * ForwardPlusRenderer — GPU-driven tiled light culling implementation.
 *
 * Algorithm overview:
 *   1. Upload all scene lights (point + spot) to SSBOs — no hard limit.
 *   2. Dispatch a compute shader with one work group per 16x16 screen tile.
 *   3. Each work group reads the depth buffer, finds min/max depth, builds
 *      a tile frustum, and tests every light against it.
 *   4. Visible light indices are written to a per-tile list in an SSBO.
 *   5. The Forward+ lit fragment shader reads the per-tile list to shade
 *      only the lights that actually affect each pixel's tile.
 */

#include "VibeEngine/Renderer/ForwardPlusRenderer.h"
#include "VibeEngine/Core/Log.h"
#include "VibeEngine/Renderer/GPUResourceTracker.h"

#include <glad/gl.h>
#include <glm/gtc/type_ptr.hpp>
#include <vector>
#include <string>
#include <cmath>

namespace VE {

// ── Static member definitions ────────────────────────────────────────

bool     ForwardPlusRenderer::s_Initialized      = false;
uint32_t ForwardPlusRenderer::s_ScreenWidth       = 0;
uint32_t ForwardPlusRenderer::s_ScreenHeight      = 0;
uint32_t ForwardPlusRenderer::s_TileCountX        = 0;
uint32_t ForwardPlusRenderer::s_TileCountY        = 0;
uint32_t ForwardPlusRenderer::s_CullComputeProgram = 0;
uint32_t ForwardPlusRenderer::s_PointLightSSBO    = 0;
uint32_t ForwardPlusRenderer::s_SpotLightSSBO     = 0;
uint32_t ForwardPlusRenderer::s_TileLightIndexSSBO = 0;
uint32_t ForwardPlusRenderer::s_NumPointLights    = 0;
uint32_t ForwardPlusRenderer::s_NumSpotLights     = 0;
bool     ForwardPlusRenderer::s_DebugHeatmap      = false;
uint32_t ForwardPlusRenderer::s_HeatmapProgram    = 0;
uint32_t ForwardPlusRenderer::s_HeatmapVAO        = 0;

// ── First-draft compute shader (unused, kept for reference) ────────

static const char* s_LightCullingComputeSrc_UNUSED = R"(
#version 460 core
layout(local_size_x = 16, local_size_y = 16) in;

// ── Uniforms ──
uniform sampler2D u_DepthTexture;
uniform mat4      u_Projection;
uniform mat4      u_View;
uniform uvec2     u_ScreenSize;
uniform uint      u_NumPointLights;
uniform uint      u_NumSpotLights;

// ── GPU light structures (must match C++ side, std430) ──
struct GPUPointLight {
    vec4 positionAndRange;   // xyz = position, w = range
    vec4 colorAndIntensity;  // xyz = color, w = intensity
    int  shadowIndex;
    float _pad0, _pad1, _pad2;
};

struct GPUSpotLight {
    vec4 posAndRange;        // xyz = position, w = range
    vec4 dirAndOuterCos;     // xyz = direction, w = cos(outerAngle)
    vec4 colorAndIntensity;  // xyz = color, w = intensity
    float innerCos;
    int  shadowIndex;
    float _pad0, _pad1;
};

// ── SSBOs ──
// Binding 1: point lights
layout(std430, binding = 1) readonly buffer PointLightBuffer {
    GPUPointLight pointLights[];
};

// Binding 2: spot lights
layout(std430, binding = 2) readonly buffer SpotLightBuffer {
    GPUSpotLight spotLights[];
};

// Binding 0: per-tile output
// Layout: [TileHeader per tile] followed by [light index pool]
// TileHeader = { int pointCount, int spotCount, int pointOffset, int spotOffset }
struct TileHeader {
    int pointLightCount;
    int spotLightCount;
    int pointLightOffset;
    int spotLightOffset;
};

layout(std430, binding = 0) buffer TileLightIndexBuffer {
    TileHeader headers[];  // first numTilesX * numTilesY entries
    // followed by int lightIndices[]
};

// We need a way to access the indices portion. We'll compute the base offset.
// The light indices start after all TileHeaders.

// ── Shared memory for depth reduction ──
shared uint s_MinDepthInt;
shared uint s_MaxDepthInt;
shared uint s_PointLightCount;
shared uint s_SpotLightCount;
shared int  s_PointLightIndices[256];
shared int  s_SpotLightIndices[256];

// ── Helper: linearize depth from depth buffer ──
float LinearizeDepth(float d, float near, float far) {
    return near * far / (far - d * (far - near));
}

// ── Helper: reconstruct view-space position from screen coords + depth ──
vec3 ScreenToView(vec2 screenCoord, float depth) {
    vec2 ndc = (screenCoord / vec2(u_ScreenSize)) * 2.0 - 1.0;
    vec4 clip = vec4(ndc, depth * 2.0 - 1.0, 1.0);
    vec4 viewPos = inverse(u_Projection) * clip;
    return viewPos.xyz / viewPos.w;
}

// ── Frustum plane from 3 points (all in view space, winding order matters) ──
// Returns vec4(normal.xyz, d) where normal . p + d >= 0 means inside
vec4 ComputePlane(vec3 p0, vec3 p1, vec3 p2) {
    vec3 v0 = p1 - p0;
    vec3 v1 = p2 - p0;
    vec3 n = normalize(cross(v0, v1));
    return vec4(n, -dot(n, p0));
}

// ── Test sphere (light) against frustum plane ──
bool SphereBehindPlane(vec3 center, float radius, vec4 plane) {
    return dot(plane.xyz, center) + plane.w < -radius;
}

void main() {
    uvec2 tileID = gl_WorkGroupID.xy;
    uint  tileIndex = tileID.y * gl_NumWorkGroups.x + tileID.x;
    uint  localIndex = gl_LocalInvocationIndex;
    uint  totalTiles = gl_NumWorkGroups.x * gl_NumWorkGroups.y;

    // Initialize shared memory
    if (localIndex == 0) {
        s_MinDepthInt = 0xFFFFFFFFu;
        s_MaxDepthInt = 0u;
        s_PointLightCount = 0u;
        s_SpotLightCount = 0u;
    }
    barrier();

    // ── Step 1: Find min/max depth in this tile ──
    uvec2 pixelCoord = tileID * uvec2(16) + gl_LocalInvocationID.xy;
    float depth = 1.0;
    if (pixelCoord.x < u_ScreenSize.x && pixelCoord.y < u_ScreenSize.y) {
        depth = texelFetch(u_DepthTexture, ivec2(pixelCoord), 0).r;
    }

    // Convert to uint for atomic min/max
    uint depthInt = floatBitsToUint(depth);
    if (depth < 1.0) { // skip sky pixels
        atomicMin(s_MinDepthInt, depthInt);
        atomicMax(s_MaxDepthInt, depthInt);
    }

    barrier();

    float minDepth = uintBitsToFloat(s_MinDepthInt);
    float maxDepth = uintBitsToFloat(s_MaxDepthInt);

    // Handle empty tiles (all sky)
    if (minDepth >= maxDepth) {
        if (localIndex == 0) {
            headers[tileIndex].pointLightCount = 0;
            headers[tileIndex].spotLightCount = 0;
            headers[tileIndex].pointLightOffset = 0;
            headers[tileIndex].spotLightOffset = 0;
        }
        return;
    }

    // ── Step 2: Construct tile frustum in view space ──
    // Tile corners in screen space
    vec2 tileMin = vec2(tileID) * 16.0;
    vec2 tileMax = min(tileMin + 16.0, vec2(u_ScreenSize));

    // Four corners of tile at near depth (minDepth) in view space
    vec3 frustumCorners[4];
    frustumCorners[0] = ScreenToView(vec2(tileMin.x, tileMin.y), minDepth);
    frustumCorners[1] = ScreenToView(vec2(tileMax.x, tileMin.y), minDepth);
    frustumCorners[2] = ScreenToView(vec2(tileMax.x, tileMax.y), minDepth);
    frustumCorners[3] = ScreenToView(vec2(tileMin.x, tileMax.y), minDepth);

    // Frustum planes (view space): left, right, top, bottom, near, far
    // Planes point inward
    vec4 frustumPlanes[6];
    // Eye is at origin in view space
    vec3 eye = vec3(0.0);
    frustumPlanes[0] = ComputePlane(eye, frustumCorners[3], frustumCorners[0]); // left
    frustumPlanes[1] = ComputePlane(eye, frustumCorners[1], frustumCorners[2]); // right
    frustumPlanes[2] = ComputePlane(eye, frustumCorners[0], frustumCorners[1]); // top
    frustumPlanes[3] = ComputePlane(eye, frustumCorners[2], frustumCorners[3]); // bottom

    // Near/far planes (in view space, looking down -Z)
    float nearZ = ScreenToView(vec2(0.5), minDepth).z;
    float farZ  = ScreenToView(vec2(0.5), maxDepth).z;
    // In view space, camera looks down -Z, so nearZ > farZ (closer = less negative)
    frustumPlanes[4] = vec4(0.0, 0.0, -1.0, -nearZ); // near plane: -z <= nearZ → z >= nearZ...
    frustumPlanes[5] = vec4(0.0, 0.0,  1.0,  farZ);  // far plane: z >= farZ

    // ── Step 3: Test point lights against tile frustum ──
    uint threadCount = 16 * 16; // 256 threads per tile
    for (uint i = localIndex; i < u_NumPointLights; i += threadCount) {
        vec4 posRange = pointLights[i].positionAndRange;
        vec3 worldPos = posRange.xyz;
        float range = posRange.w;

        // Transform to view space
        vec3 viewPos = (u_View * vec4(worldPos, 1.0)).xyz;

        // Test against all 6 frustum planes
        bool visible = true;
        for (int p = 0; p < 6; ++p) {
            if (SphereBehindPlane(viewPos, range, frustumPlanes[p])) {
                visible = false;
                break;
            }
        }

        if (visible) {
            uint idx = atomicAdd(s_PointLightCount, 1u);
            if (idx < 256u)
                s_PointLightIndices[idx] = int(i);
        }
    }

    // ── Step 4: Test spot lights against tile frustum ──
    for (uint i = localIndex; i < u_NumSpotLights; i += threadCount) {
        vec4 posRange = spotLights[i].posAndRange;
        vec3 worldPos = posRange.xyz;
        float range = posRange.w;

        // Transform to view space
        vec3 viewPos = (u_View * vec4(worldPos, 1.0)).xyz;

        // Treat spot light as sphere for culling (conservative)
        bool visible = true;
        for (int p = 0; p < 6; ++p) {
            if (SphereBehindPlane(viewPos, range, frustumPlanes[p])) {
                visible = false;
                break;
            }
        }

        if (visible) {
            uint idx = atomicAdd(s_SpotLightCount, 1u);
            if (idx < 256u)
                s_SpotLightIndices[idx] = int(i);
        }
    }

    barrier();

    // ── Step 5: Write results to SSBO ──
    // Each tile gets MAX_LIGHTS_PER_TILE slots for point + spot indices
    // Layout in the index region: tile0_point[256] tile0_spot[256] tile1_point[256] ...
    // But headers[] occupies the first totalTiles entries of the SSBO.
    // We use a flat int array after all the headers.

    if (localIndex == 0) {
        uint pointCount = min(s_PointLightCount, 256u);
        uint spotCount  = min(s_SpotLightCount, 256u);

        headers[tileIndex].pointLightCount  = int(pointCount);
        headers[tileIndex].spotLightCount   = int(spotCount);

        // Each tile has 512 int slots (256 point + 256 spot)
        // Offset in ints from the start of the indices region
        uint baseOffset = tileIndex * 512u;
        headers[tileIndex].pointLightOffset = int(baseOffset);
        headers[tileIndex].spotLightOffset  = int(baseOffset + 256u);
    }

    barrier();

    // Write light indices (all threads participate)
    uint pointCount = min(s_PointLightCount, 256u);
    uint spotCount  = min(s_SpotLightCount, 256u);
    uint baseOffset = tileIndex * 512u;

    // Point light indices
    for (uint i = localIndex; i < pointCount; i += threadCount) {
        // We need to write into the indices region which starts after totalTiles TileHeaders.
        // Each TileHeader is 4 ints = 16 bytes.
        // But our SSBO has headers[] as TileHeader[], so we need a separate access.
        // We'll use the raw buffer offset approach:
        // The indices start at byte offset: totalTiles * sizeof(TileHeader) = totalTiles * 16
        // In terms of int offsets: totalTiles * 4
        // But since we declared headers[] as TileHeader[], we can't directly index ints after it.
        // Solution: we'll store indices in a second SSBO or use a different layout.
        // Actually, let's re-think: use a single buffer with raw int layout.
        // We'll reinterpret everything.
        // For simplicity, we'll output in the same SSBO using raw byte addressing.
        // GLSL doesn't support flexible array members after a struct array easily.
        // Let's use a separate SSBO for indices.

        // Actually: we declared the buffer with just headers[]. In std430, GLSL only allows
        // the last member to be unsized. We need to restructure.
        // The pragmatic solution: use two separate buffer bindings for the tile data.
        // But that wastes a binding. Instead, let's just use a flat int[] buffer
        // and pack everything ourselves.
    }
    // (See revised approach below — the buffer is declared as flat ints)
}
)";

// ── REVISED compute shader with flat int buffer layout ──────────────

static const char* s_LightCullingComputeSrcV2 = R"(
#version 460 core
layout(local_size_x = 16, local_size_y = 16) in;

uniform sampler2D u_DepthTexture;
uniform mat4      u_Projection;
uniform mat4      u_View;
uniform uvec2     u_ScreenSize;
uniform uint      u_NumPointLights;
uniform uint      u_NumSpotLights;
uniform uint      u_TileCountX;

struct GPUPointLight {
    vec4 positionAndRange;
    vec4 colorAndIntensity;
    int  shadowIndex;
    float _pad0, _pad1, _pad2;
};

struct GPUSpotLight {
    vec4 posAndRange;
    vec4 dirAndOuterCos;
    vec4 colorAndIntensity;
    float innerCos;
    int  shadowIndex;
    float _pad0, _pad1;
};

layout(std430, binding = 1) readonly buffer PointLightBuffer {
    GPUPointLight pointLights[];
};

layout(std430, binding = 2) readonly buffer SpotLightBuffer {
    GPUSpotLight spotLights[];
};

// Binding 0: flat int buffer
// Per-tile layout (520 ints per tile):
//   [0]   = pointLightCount
//   [1]   = spotLightCount
//   [2]   = (reserved)
//   [3]   = (reserved)
//   [4..259]   = point light indices (max 256)
//   [260..515] = spot light indices (max 256)
// Total per tile: 4 + 256 + 256 = 516 ints, padded to 520 for alignment
const uint TILE_STRIDE = 520u;

layout(std430, binding = 0) buffer TileLightIndexBuffer {
    int tileData[];
};

shared uint s_MinDepthInt;
shared uint s_MaxDepthInt;
shared uint s_PointLightCount;
shared uint s_SpotLightCount;
shared int  s_PointLightIndices[256];
shared int  s_SpotLightIndices[256];

vec3 ScreenToView(vec2 screenCoord, float depth) {
    vec2 ndc = (screenCoord / vec2(u_ScreenSize)) * 2.0 - 1.0;
    vec4 clip = vec4(ndc, depth * 2.0 - 1.0, 1.0);
    vec4 viewPos = inverse(u_Projection) * clip;
    return viewPos.xyz / viewPos.w;
}

vec4 ComputePlane(vec3 p0, vec3 p1, vec3 p2) {
    vec3 v0 = p1 - p0;
    vec3 v1 = p2 - p0;
    vec3 n = normalize(cross(v0, v1));
    return vec4(n, -dot(n, p0));
}

bool SphereBehindPlane(vec3 center, float radius, vec4 plane) {
    return dot(plane.xyz, center) + plane.w < -radius;
}

void main() {
    uvec2 tileID = gl_WorkGroupID.xy;
    uint  tileIndex = tileID.y * u_TileCountX + tileID.x;
    uint  localIndex = gl_LocalInvocationIndex;
    uint  tileBase = tileIndex * TILE_STRIDE;

    if (localIndex == 0) {
        s_MinDepthInt = 0xFFFFFFFFu;
        s_MaxDepthInt = 0u;
        s_PointLightCount = 0u;
        s_SpotLightCount = 0u;
    }
    barrier();

    // Step 1: min/max depth
    uvec2 pixelCoord = tileID * uvec2(16) + gl_LocalInvocationID.xy;
    float depth = 1.0;
    if (pixelCoord.x < u_ScreenSize.x && pixelCoord.y < u_ScreenSize.y) {
        depth = texelFetch(u_DepthTexture, ivec2(pixelCoord), 0).r;
    }

    uint depthInt = floatBitsToUint(depth);
    if (depth < 1.0) {
        atomicMin(s_MinDepthInt, depthInt);
        atomicMax(s_MaxDepthInt, depthInt);
    }
    barrier();

    float minDepth = uintBitsToFloat(s_MinDepthInt);
    float maxDepth = uintBitsToFloat(s_MaxDepthInt);

    if (minDepth >= maxDepth) {
        if (localIndex == 0) {
            tileData[tileBase + 0] = 0;
            tileData[tileBase + 1] = 0;
        }
        return;
    }

    // Step 2: tile frustum in view space
    vec2 tileMin = vec2(tileID) * 16.0;
    vec2 tileMax = min(tileMin + 16.0, vec2(u_ScreenSize));

    vec3 corners[4];
    corners[0] = ScreenToView(vec2(tileMin.x, tileMin.y), minDepth);
    corners[1] = ScreenToView(vec2(tileMax.x, tileMin.y), minDepth);
    corners[2] = ScreenToView(vec2(tileMax.x, tileMax.y), minDepth);
    corners[3] = ScreenToView(vec2(tileMin.x, tileMax.y), minDepth);

    vec3 eye = vec3(0.0);
    vec4 frustumPlanes[6];
    frustumPlanes[0] = ComputePlane(eye, corners[3], corners[0]); // left
    frustumPlanes[1] = ComputePlane(eye, corners[1], corners[2]); // right
    frustumPlanes[2] = ComputePlane(eye, corners[0], corners[1]); // top
    frustumPlanes[3] = ComputePlane(eye, corners[2], corners[3]); // bottom

    float nearZ = ScreenToView(vec2(0.5), minDepth).z;
    float farZ  = ScreenToView(vec2(0.5), maxDepth).z;
    // View space: camera looks -Z. near is less negative than far.
    // near plane: dot(0,0,-1, p) + nearZ >= 0 → -pz + nearZ >= 0 → pz <= nearZ
    frustumPlanes[4] = vec4(0.0, 0.0, -1.0, nearZ);
    // far plane: dot(0,0,1, p) - farZ >= 0 → pz - farZ >= 0...
    // Actually: pz >= farZ (farZ is more negative)
    frustumPlanes[5] = vec4(0.0, 0.0, 1.0, -farZ);

    uint threadCount = 256u;

    // Step 3: cull point lights
    for (uint i = localIndex; i < u_NumPointLights; i += threadCount) {
        vec3 worldPos = pointLights[i].positionAndRange.xyz;
        float range = pointLights[i].positionAndRange.w;
        vec3 viewPos = (u_View * vec4(worldPos, 1.0)).xyz;

        bool visible = true;
        for (int p = 0; p < 6; ++p) {
            if (SphereBehindPlane(viewPos, range, frustumPlanes[p])) {
                visible = false;
                break;
            }
        }

        if (visible) {
            uint idx = atomicAdd(s_PointLightCount, 1u);
            if (idx < 256u)
                s_PointLightIndices[idx] = int(i);
        }
    }

    // Step 4: cull spot lights
    for (uint i = localIndex; i < u_NumSpotLights; i += threadCount) {
        vec3 worldPos = spotLights[i].posAndRange.xyz;
        float range = spotLights[i].posAndRange.w;
        vec3 viewPos = (u_View * vec4(worldPos, 1.0)).xyz;

        bool visible = true;
        for (int p = 0; p < 6; ++p) {
            if (SphereBehindPlane(viewPos, range, frustumPlanes[p])) {
                visible = false;
                break;
            }
        }

        if (visible) {
            uint idx = atomicAdd(s_SpotLightCount, 1u);
            if (idx < 256u)
                s_SpotLightIndices[idx] = int(i);
        }
    }

    barrier();

    // Step 5: write results
    uint pointCount = min(s_PointLightCount, 256u);
    uint spotCount  = min(s_SpotLightCount, 256u);

    if (localIndex == 0) {
        tileData[tileBase + 0] = int(pointCount);
        tileData[tileBase + 1] = int(spotCount);
        tileData[tileBase + 2] = 0; // reserved
        tileData[tileBase + 3] = 0; // reserved
    }

    barrier();

    // Write point light indices (threads cooperate)
    for (uint i = localIndex; i < pointCount; i += threadCount) {
        tileData[tileBase + 4u + i] = s_PointLightIndices[i];
    }

    // Write spot light indices
    for (uint i = localIndex; i < spotCount; i += threadCount) {
        tileData[tileBase + 260u + i] = s_SpotLightIndices[i];
    }
}
)";

// ── Heatmap debug shader ─────────────────────────────────────────────

static const char* s_HeatmapVertSrc = R"(
#version 460 core
out vec2 v_TexCoord;
void main() {
    // Full-screen triangle
    vec2 pos = vec2((gl_VertexID & 1) * 2.0 - 1.0, (gl_VertexID >> 1) * 2.0 - 1.0);
    // We need 3 vertices for a triangle covering the screen
    float x = -1.0 + float((gl_VertexID & 1) << 2);
    float y = -1.0 + float((gl_VertexID & 2) << 1);
    gl_Position = vec4(x, y, 0.0, 1.0);
    v_TexCoord = vec2(x, y) * 0.5 + 0.5;
}
)";

static const char* s_HeatmapFragSrc = R"(
#version 460 core
in vec2 v_TexCoord;
out vec4 FragColor;

uniform uvec2 u_ScreenSize;
uniform uint  u_TileCountX;

const uint TILE_STRIDE = 520u;

layout(std430, binding = 0) readonly buffer TileLightIndexBuffer {
    int tileData[];
};

vec3 HeatmapColor(float t) {
    // Blue -> Green -> Yellow -> Red
    t = clamp(t, 0.0, 1.0);
    if (t < 0.25) return mix(vec3(0, 0, 1), vec3(0, 1, 1), t * 4.0);
    if (t < 0.50) return mix(vec3(0, 1, 1), vec3(0, 1, 0), (t - 0.25) * 4.0);
    if (t < 0.75) return mix(vec3(0, 1, 0), vec3(1, 1, 0), (t - 0.50) * 4.0);
    return mix(vec3(1, 1, 0), vec3(1, 0, 0), (t - 0.75) * 4.0);
}

void main() {
    uvec2 pixel = uvec2(v_TexCoord * vec2(u_ScreenSize));
    uvec2 tileID = pixel / uvec2(16);
    uint tileIndex = tileID.y * u_TileCountX + tileID.x;
    uint tileBase = tileIndex * TILE_STRIDE;

    int pointCount = tileData[tileBase + 0];
    int spotCount  = tileData[tileBase + 1];
    int totalLights = pointCount + spotCount;

    float t = float(totalLights) / 64.0; // 64 lights = fully red
    vec3 color = HeatmapColor(t);

    FragColor = vec4(color, 0.4); // semi-transparent overlay
}
)";

// ── Helper: compile a single shader stage ──

static GLuint CompileShaderStage(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLint len;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> log(static_cast<size_t>(len));
        glGetShaderInfoLog(shader, len, &len, log.data());
        VE_ENGINE_ERROR("ForwardPlus shader compile error: {}", log.data());
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

// ── Init / Shutdown ─────────────────────────────────────────────────

void ForwardPlusRenderer::Init(uint32_t screenWidth, uint32_t screenHeight) {
    if (s_Initialized) return;

    s_ScreenWidth  = screenWidth;
    s_ScreenHeight = screenHeight;
    s_TileCountX = (screenWidth  + TILE_SIZE - 1) / TILE_SIZE;
    s_TileCountY = (screenHeight + TILE_SIZE - 1) / TILE_SIZE;

    CompileCullShader();
    CompileHeatmapShader();
    CreateSSBOs();

    s_Initialized = true;
    VE_ENGINE_INFO("ForwardPlusRenderer initialized: {}x{} tiles ({}x{} pixels)",
                   s_TileCountX, s_TileCountY, screenWidth, screenHeight);
}

void ForwardPlusRenderer::Shutdown() {
    if (!s_Initialized) return;

    if (s_CullComputeProgram) { glDeleteProgram(s_CullComputeProgram); s_CullComputeProgram = 0; }
    if (s_PointLightSSBO) { VE_GPU_UNTRACK(GPUResourceType::VertexBuffer, s_PointLightSSBO); glDeleteBuffers(1, &s_PointLightSSBO); s_PointLightSSBO = 0; }
    if (s_SpotLightSSBO)  { VE_GPU_UNTRACK(GPUResourceType::VertexBuffer, s_SpotLightSSBO);  glDeleteBuffers(1, &s_SpotLightSSBO);  s_SpotLightSSBO = 0; }
    if (s_TileLightIndexSSBO) { VE_GPU_UNTRACK(GPUResourceType::VertexBuffer, s_TileLightIndexSSBO); glDeleteBuffers(1, &s_TileLightIndexSSBO); s_TileLightIndexSSBO = 0; }
    if (s_HeatmapProgram) { glDeleteProgram(s_HeatmapProgram); s_HeatmapProgram = 0; }
    if (s_HeatmapVAO) { glDeleteVertexArrays(1, &s_HeatmapVAO); s_HeatmapVAO = 0; }

    s_Initialized = false;
    VE_ENGINE_INFO("ForwardPlusRenderer shut down");
}

void ForwardPlusRenderer::Resize(uint32_t screenWidth, uint32_t screenHeight) {
    if (screenWidth == 0 || screenHeight == 0) return;
    if (screenWidth == s_ScreenWidth && screenHeight == s_ScreenHeight) return;

    s_ScreenWidth  = screenWidth;
    s_ScreenHeight = screenHeight;
    s_TileCountX = (screenWidth  + TILE_SIZE - 1) / TILE_SIZE;
    s_TileCountY = (screenHeight + TILE_SIZE - 1) / TILE_SIZE;

    // Recreate tile index SSBO with new size
    if (s_TileLightIndexSSBO) {
        VE_GPU_UNTRACK(GPUResourceType::VertexBuffer, s_TileLightIndexSSBO);
        glDeleteBuffers(1, &s_TileLightIndexSSBO);
        s_TileLightIndexSSBO = 0;
    }

    uint32_t totalTiles = s_TileCountX * s_TileCountY;
    // Each tile needs TILE_STRIDE ints (520 * 4 bytes)
    uint32_t tileBufferSize = totalTiles * 520 * sizeof(int);

    glGenBuffers(1, &s_TileLightIndexSSBO);
    VE_GPU_TRACK(GPUResourceType::VertexBuffer, s_TileLightIndexSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_TileLightIndexSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER, tileBufferSize, nullptr, GL_DYNAMIC_COPY);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    VE_ENGINE_TRACE("ForwardPlus resized: {}x{} tiles", s_TileCountX, s_TileCountY);
}

void ForwardPlusRenderer::CompileCullShader() {
    GLuint cs = CompileShaderStage(GL_COMPUTE_SHADER, s_LightCullingComputeSrcV2);
    if (!cs) {
        VE_ENGINE_ERROR("ForwardPlus: failed to compile light culling compute shader");
        return;
    }

    s_CullComputeProgram = glCreateProgram();
    glAttachShader(s_CullComputeProgram, cs);
    glLinkProgram(s_CullComputeProgram);

    GLint success;
    glGetProgramiv(s_CullComputeProgram, GL_LINK_STATUS, &success);
    if (!success) {
        GLint len;
        glGetProgramiv(s_CullComputeProgram, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> log(static_cast<size_t>(len));
        glGetProgramInfoLog(s_CullComputeProgram, len, &len, log.data());
        VE_ENGINE_ERROR("ForwardPlus: compute shader link error: {}", log.data());
        glDeleteProgram(s_CullComputeProgram);
        s_CullComputeProgram = 0;
    }

    glDeleteShader(cs);
}

void ForwardPlusRenderer::CompileHeatmapShader() {
    GLuint vs = CompileShaderStage(GL_VERTEX_SHADER, s_HeatmapVertSrc);
    GLuint fs = CompileShaderStage(GL_FRAGMENT_SHADER, s_HeatmapFragSrc);
    if (!vs || !fs) return;

    s_HeatmapProgram = glCreateProgram();
    glAttachShader(s_HeatmapProgram, vs);
    glAttachShader(s_HeatmapProgram, fs);
    glLinkProgram(s_HeatmapProgram);

    GLint success;
    glGetProgramiv(s_HeatmapProgram, GL_LINK_STATUS, &success);
    if (!success) {
        GLint len;
        glGetProgramiv(s_HeatmapProgram, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> log(static_cast<size_t>(len));
        glGetProgramInfoLog(s_HeatmapProgram, len, &len, log.data());
        VE_ENGINE_ERROR("ForwardPlus: heatmap shader link error: {}", log.data());
    }

    glDeleteShader(vs);
    glDeleteShader(fs);

    // Empty VAO for full-screen triangle
    glGenVertexArrays(1, &s_HeatmapVAO);
}

void ForwardPlusRenderer::CreateSSBOs() {
    // Point light SSBO — start with space for 1024 lights, will grow dynamically
    glGenBuffers(1, &s_PointLightSSBO);
    VE_GPU_TRACK(GPUResourceType::VertexBuffer, s_PointLightSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_PointLightSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER, 1024 * sizeof(GPUPointLight), nullptr, GL_DYNAMIC_DRAW);

    // Spot light SSBO
    glGenBuffers(1, &s_SpotLightSSBO);
    VE_GPU_TRACK(GPUResourceType::VertexBuffer, s_SpotLightSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_SpotLightSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER, 512 * sizeof(GPUSpotLight), nullptr, GL_DYNAMIC_DRAW);

    // Tile light index SSBO
    uint32_t totalTiles = s_TileCountX * s_TileCountY;
    uint32_t tileBufferSize = totalTiles * 520 * sizeof(int);

    glGenBuffers(1, &s_TileLightIndexSSBO);
    VE_GPU_TRACK(GPUResourceType::VertexBuffer, s_TileLightIndexSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_TileLightIndexSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER, tileBufferSize, nullptr, GL_DYNAMIC_COPY);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

// ── Upload lights ───────────────────────────────────────────────────

void ForwardPlusRenderer::UploadLights(const std::vector<GPUPointLight>& pointLights,
                                        const std::vector<GPUSpotLight>& spotLights) {
    s_NumPointLights = static_cast<uint32_t>(pointLights.size());
    s_NumSpotLights  = static_cast<uint32_t>(spotLights.size());

    // Point lights
    if (!pointLights.empty()) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_PointLightSSBO);
        GLint currentSize = 0;
        glGetBufferParameteriv(GL_SHADER_STORAGE_BUFFER, GL_BUFFER_SIZE, &currentSize);
        GLsizeiptr needed = static_cast<GLsizeiptr>(pointLights.size() * sizeof(GPUPointLight));
        if (needed > currentSize) {
            glBufferData(GL_SHADER_STORAGE_BUFFER, needed, pointLights.data(), GL_DYNAMIC_DRAW);
        } else {
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, needed, pointLights.data());
        }
    }

    // Spot lights
    if (!spotLights.empty()) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_SpotLightSSBO);
        GLint currentSize = 0;
        glGetBufferParameteriv(GL_SHADER_STORAGE_BUFFER, GL_BUFFER_SIZE, &currentSize);
        GLsizeiptr needed = static_cast<GLsizeiptr>(spotLights.size() * sizeof(GPUSpotLight));
        if (needed > currentSize) {
            glBufferData(GL_SHADER_STORAGE_BUFFER, needed, spotLights.data(), GL_DYNAMIC_DRAW);
        } else {
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, needed, spotLights.data());
        }
    }

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

// ── Cull lights ─────────────────────────────────────────────────────

void ForwardPlusRenderer::CullLights(uint32_t depthTexture,
                                      const glm::mat4& projection,
                                      const glm::mat4& view,
                                      uint32_t screenWidth,
                                      uint32_t screenHeight) {
    if (!s_CullComputeProgram) return;

    // Auto-resize if needed
    Resize(screenWidth, screenHeight);

    glUseProgram(s_CullComputeProgram);

    // Bind depth texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, depthTexture);
    glUniform1i(glGetUniformLocation(s_CullComputeProgram, "u_DepthTexture"), 0);

    // Set uniforms
    glUniformMatrix4fv(glGetUniformLocation(s_CullComputeProgram, "u_Projection"),
                       1, GL_FALSE, glm::value_ptr(projection));
    glUniformMatrix4fv(glGetUniformLocation(s_CullComputeProgram, "u_View"),
                       1, GL_FALSE, glm::value_ptr(view));
    glUniform2ui(glGetUniformLocation(s_CullComputeProgram, "u_ScreenSize"),
                 screenWidth, screenHeight);
    glUniform1ui(glGetUniformLocation(s_CullComputeProgram, "u_NumPointLights"), s_NumPointLights);
    glUniform1ui(glGetUniformLocation(s_CullComputeProgram, "u_NumSpotLights"), s_NumSpotLights);
    glUniform1ui(glGetUniformLocation(s_CullComputeProgram, "u_TileCountX"), s_TileCountX);

    // Bind SSBOs
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, s_TileLightIndexSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, s_PointLightSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, s_SpotLightSSBO);

    // Dispatch: one work group per tile
    glDispatchCompute(s_TileCountX, s_TileCountY, 1);

    // Memory barrier: ensure SSBO writes are visible to fragment shaders
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    glUseProgram(0);
}

// ── Bind / Unbind ───────────────────────────────────────────────────

void ForwardPlusRenderer::BindLightData() {
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, s_TileLightIndexSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, s_PointLightSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, s_SpotLightSSBO);
}

void ForwardPlusRenderer::UnbindLightData() {
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, 0);
}

// ── Debug Heatmap ───────────────────────────────────────────────────

void ForwardPlusRenderer::DrawDebugHeatmap(uint32_t screenWidth, uint32_t screenHeight) {
    if (!s_HeatmapProgram || !s_HeatmapVAO) return;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);

    glUseProgram(s_HeatmapProgram);
    glUniform2ui(glGetUniformLocation(s_HeatmapProgram, "u_ScreenSize"),
                 screenWidth, screenHeight);
    glUniform1ui(glGetUniformLocation(s_HeatmapProgram, "u_TileCountX"), s_TileCountX);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, s_TileLightIndexSSBO);

    glBindVertexArray(s_HeatmapVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glUseProgram(0);
    glEnable(GL_DEPTH_TEST);
}

} // namespace VE
