/*
 * Terrain — Heightmap-based terrain generation and rendering.
 *
 * Generates a subdivided grid mesh from a heightmap (image or procedural).
 * Supports height-based multi-texture splatting (up to 4 layers) with
 * configurable blend heights and tiling.
 */
#pragma once

#include "VibeEngine/Renderer/VertexArray.h"
#include "VibeEngine/Renderer/Texture.h"
#include "VibeEngine/Renderer/Shader.h"
#include "VibeEngine/Renderer/Material.h"

#include <string>
#include <vector>
#include <array>
#include <memory>

namespace VE {

struct TerrainTextureLayer {
    std::string TexturePath;
    std::shared_ptr<Texture2D> Texture;
    float TilingScale = 10.0f;   // UV tiling multiplier
    float BlendStart  = 0.0f;    // height at which this layer starts (0-1 normalized)
    float BlendEnd    = 0.25f;   // height at which this layer is fully visible
};

class Terrain {
public:
    Terrain() = default;
    ~Terrain() = default;

    // Generate mesh from heightmap data (values in 0-1 range)
    void Generate(int resolution, float worldSizeX, float worldSizeZ,
                  float heightScale, const std::vector<float>& heightmap);

    // Generate procedural terrain using Perlin-like noise
    void GenerateProcedural(int resolution, float worldSizeX, float worldSizeZ,
                            float heightScale, int octaves = 4, float persistence = 0.5f,
                            float lacunarity = 2.0f, float noiseScale = 50.0f, int seed = 42);

    // Generate from grayscale heightmap image
    bool GenerateFromImage(const std::string& heightmapPath,
                           float worldSizeX, float worldSizeZ, float heightScale);

    // Access
    const std::shared_ptr<VertexArray>& GetMesh() const { return m_VAO; }
    const std::vector<float>& GetHeightmap() const { return m_Heightmap; }
    int GetResolution() const { return m_Resolution; }
    float GetHeightAt(float worldX, float worldZ) const;

    // Rebuild mesh (call after modifying heightmap externally)
    void RebuildMesh();

private:
    void BuildMeshFromHeightmap();
    glm::vec3 ComputeNormal(int x, int z) const;

    std::shared_ptr<VertexArray> m_VAO;
    std::vector<float> m_Heightmap; // row-major, resolution x resolution
    int m_Resolution = 0;
    float m_WorldSizeX = 100.0f;
    float m_WorldSizeZ = 100.0f;
    float m_HeightScale = 10.0f;
};

} // namespace VE
