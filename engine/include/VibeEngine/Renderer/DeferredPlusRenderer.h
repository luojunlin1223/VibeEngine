/*
 * DeferredPlusRenderer — Tiled Deferred Rendering with compute-shader light culling.
 *
 * Design overview:
 *   1. G-Buffer pass: renders all opaque geometry into 4 MRT textures
 *      (Position+Metallic, Normal+Roughness, Albedo+AO, Emission+Flags)
 *   2. Light upload: packs all point/spot lights into SSBOs (no limit)
 *   3. Tile culling: a compute shader partitions the screen into 16x16 tiles,
 *      finds min/max depth per tile, builds tile frustum planes, and tests
 *      each light against the frustum.  Result: per-tile light index lists.
 *   4. Tiled lighting pass: fullscreen quad reads G-Buffer + per-tile light
 *      lists and evaluates PBR lighting only for visible lights per tile.
 *
 * OpenGL 4.6 only.  All shaders are embedded as inline strings.
 */
#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include <glm/glm.hpp>

namespace VE {

class Scene;
class Shader;

// GPU-side light structs — 16-byte aligned for std430
struct GPUPointLight {
    glm::vec4 positionAndRange;    // xyz = world position, w = range
    glm::vec4 colorAndIntensity;   // rgb = color, a = intensity
    int       shadowIndex;         // -1 = no shadow
    float     padding[3];
};

struct GPUSpotLight {
    glm::vec4 posAndRange;         // xyz = world position, w = range
    glm::vec4 dirAndOuterCos;      // xyz = direction, w = cos(outerAngle)
    glm::vec4 colorAndIntensity;   // rgb = color, a = intensity
    float     innerCos;            // cos(innerAngle)
    int       shadowIndex;         // -1 = no shadow
    float     padding[2];
};

// Per-frame stats exposed for the profiler panel
struct TiledLightingStats {
    uint32_t TileCountX     = 0;
    uint32_t TileCountY     = 0;
    uint32_t TotalTiles     = 0;
    uint32_t NumPointLights = 0;
    uint32_t NumSpotLights  = 0;
    bool     DebugOverlay   = false;
};

class DeferredPlusRenderer {
public:
    static constexpr uint32_t TILE_SIZE = 16;
    // Max light indices stored per tile
    static constexpr uint32_t MAX_LIGHTS_PER_TILE = 256;

    DeferredPlusRenderer() = default;
    ~DeferredPlusRenderer();

    void Init(uint32_t width, uint32_t height);
    void Shutdown();
    void Resize(uint32_t width, uint32_t height);

    // ── Per-frame pipeline steps (called from Scene) ─────────────────

    // Step 1: Bind G-Buffer FBO for geometry pass
    void BeginGBufferPass();
    void EndGBufferPass();

    // Step 2: Upload point/spot lights from Scene into SSBOs
    void UploadLights(Scene& scene);

    // Step 3: Run compute-shader tile culling
    void CullLights(const glm::mat4& projection, const glm::mat4& view);

    // Step 4: Tiled deferred lighting pass (fullscreen quad)
    //         Writes to the currently bound framebuffer (caller's HDR FBO).
    void LightingPass(const glm::mat4& view, const glm::mat4& projection,
                      const glm::vec3& cameraPos, Scene& scene);

    // ── Accessors ────────────────────────────────────────────────────

    uint32_t GetGBufferFBO() const { return m_GBufferFBO; }
    uint32_t GetGBufferDepthTexture() const { return m_DepthTexture; }
    uint32_t GetGBufferPositionTexture() const { return m_GBufferTextures[0]; }
    uint32_t GetGBufferNormalTexture() const   { return m_GBufferTextures[1]; }
    uint32_t GetGBufferAlbedoTexture() const   { return m_GBufferTextures[2]; }
    uint32_t GetGBufferEmissionTexture() const { return m_GBufferTextures[3]; }

    const TiledLightingStats& GetStats() const { return m_Stats; }
    TiledLightingStats& GetStats() { return m_Stats; }

    bool IsInitialized() const { return m_Initialized; }

    // Toggle debug overlay (tiles colored by light count)
    void SetDebugOverlay(bool enabled) { m_Stats.DebugOverlay = enabled; }

private:
    void CreateGBuffer();
    void DestroyGBuffer();
    void CreateSSBOs();
    void DestroySSBOs();
    void CompileShaders();
    void DestroyShaders();
    void RecalculateTileGrid();

    bool m_Initialized = false;
    uint32_t m_Width = 0, m_Height = 0;

    // G-Buffer
    uint32_t m_GBufferFBO = 0;
    uint32_t m_GBufferTextures[4] = {}; // Position+Metal, Normal+Rough, Albedo+AO, Emission+Flags
    uint32_t m_DepthTexture = 0;

    // SSBOs
    uint32_t m_PointLightSSBO  = 0;  // binding 0
    uint32_t m_SpotLightSSBO   = 0;  // binding 1
    uint32_t m_LightIndexSSBO  = 0;  // binding 2 — per-tile light indices
    uint32_t m_TileInfoSSBO    = 0;  // binding 3 — per-tile offset+count

    // Tile grid
    uint32_t m_TileCountX = 0;
    uint32_t m_TileCountY = 0;
    uint32_t m_TotalTiles = 0;

    // Shaders (raw GL programs — not using engine Shader class for compute)
    uint32_t m_CullComputeProgram = 0;
    uint32_t m_LightingProgram    = 0;
    uint32_t m_QuadVAO = 0; // empty VAO for fullscreen triangle

    // Light data (CPU side, uploaded each frame)
    std::vector<GPUPointLight> m_PointLights;
    std::vector<GPUSpotLight>  m_SpotLights;

    TiledLightingStats m_Stats;
};

} // namespace VE
