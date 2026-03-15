/*
 * ForwardPlusRenderer — Tiled Forward (Forward+) Rendering Pipeline
 *
 * Implements GPU-driven light culling via compute shaders. The screen is divided
 * into 16x16 pixel tiles. A compute shader determines the min/max depth per tile,
 * constructs a frustum, and tests each light against it. The resulting per-tile
 * light index lists are stored in SSBOs and read by the lit fragment shader,
 * removing the hard-coded light limit of the traditional forward path.
 *
 * Key resources:
 *   - SSBO 0: Per-tile light index list + offsets
 *   - SSBO 1: GPU point light array
 *   - SSBO 2: GPU spot light array
 */
#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <vector>
#include <memory>

namespace VE {

class Shader;

// GPU-side point light (16-byte aligned for std430)
struct GPUPointLight {
    glm::vec4 PositionAndRange;     // xyz = world position, w = range
    glm::vec4 ColorAndIntensity;    // xyz = color, w = intensity
};

// GPU-side spot light (16-byte aligned for std430)
struct GPUSpotLight {
    glm::vec4 PosAndRange;          // xyz = world position, w = range
    glm::vec4 DirAndInnerCos;       // xyz = direction, w = cos(innerAngle)
    glm::vec4 ColorAndIntensity;    // xyz = color, w = intensity
    float     OuterCos;             // cos(outerAngle)
    float     _pad0, _pad1, _pad2;  // pad to 16-byte alignment
};

// Per-tile header stored at the beginning of the tile light index buffer
struct TileHeader {
    int PointLightCount;
    int SpotLightCount;
    int PointLightOffset; // index into the global light index array
    int SpotLightOffset;
};

class ForwardPlusRenderer {
public:
    static constexpr int TILE_SIZE = 16;
    static constexpr int MAX_LIGHTS_PER_TILE = 256;

    // Initialize GPU resources. Call once after GL context is ready.
    static void Init(uint32_t screenWidth, uint32_t screenHeight);

    // Shutdown and release all GPU resources.
    static void Shutdown();

    // Resize tile grid when viewport changes.
    static void Resize(uint32_t screenWidth, uint32_t screenHeight);

    // Upload light data from CPU arrays into SSBOs.
    static void UploadLights(const std::vector<GPUPointLight>& pointLights,
                             const std::vector<GPUSpotLight>& spotLights);

    // Dispatch the light culling compute shader.
    // depthTexture: the GL texture ID of the scene depth buffer
    // projection/view: camera matrices for frustum reconstruction
    static void CullLights(uint32_t depthTexture,
                           const glm::mat4& projection,
                           const glm::mat4& view,
                           uint32_t screenWidth,
                           uint32_t screenHeight);

    // Bind the light SSBOs so that the Forward+ lit shader can read them.
    // Call this before drawing geometry with the LitForwardPlus shader.
    static void BindLightData();

    // Unbind SSBOs (optional cleanup).
    static void UnbindLightData();

    // Query
    static bool IsInitialized() { return s_Initialized; }
    static uint32_t GetTileCountX() { return s_TileCountX; }
    static uint32_t GetTileCountY() { return s_TileCountY; }
    static uint32_t GetTotalPointLights() { return s_NumPointLights; }
    static uint32_t GetTotalSpotLights() { return s_NumSpotLights; }

    // Debug: enable/disable tile heatmap overlay
    static void SetDebugTileHeatmap(bool enabled) { s_DebugHeatmap = enabled; }
    static bool GetDebugTileHeatmap() { return s_DebugHeatmap; }

    // Draw a debug heatmap overlay showing light count per tile
    static void DrawDebugHeatmap(uint32_t screenWidth, uint32_t screenHeight);

private:
    static bool     s_Initialized;
    static uint32_t s_ScreenWidth;
    static uint32_t s_ScreenHeight;
    static uint32_t s_TileCountX;
    static uint32_t s_TileCountY;

    // Compute shader program
    static uint32_t s_CullComputeProgram;

    // SSBOs
    static uint32_t s_PointLightSSBO;      // binding 1
    static uint32_t s_SpotLightSSBO;       // binding 2
    static uint32_t s_TileLightIndexSSBO;  // binding 0 — contains TileHeaders + light indices

    // Light counts (CPU side, for stats)
    static uint32_t s_NumPointLights;
    static uint32_t s_NumSpotLights;

    // Debug
    static bool     s_DebugHeatmap;
    static uint32_t s_HeatmapProgram;
    static uint32_t s_HeatmapVAO;

    static void CreateSSBOs();
    static void CompileCullShader();
    static void CompileHeatmapShader();
};

} // namespace VE
