/*
 * Terrain — Heightmap mesh generation with procedural noise support.
 *
 * Vertex layout matches Lit shader: pos(3)+normal(3)+color(3)+uv(2) = 11 floats.
 * Normals are computed from finite differences on the heightmap.
 */
#include "VibeEngine/Terrain/Terrain.h"
#include "VibeEngine/Renderer/Buffer.h"
#include "VibeEngine/Core/Log.h"

#include <glm/glm.hpp>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <numeric>

// stb_image for heightmap loading
#include <stb_image.h>

namespace VE {

// ── Simple value noise (deterministic, seedable) ─────────────────────

static float Hash2D(int x, int y, int seed) {
    int n = x + y * 57 + seed * 131;
    n = (n << 13) ^ n;
    return 1.0f - ((n * (n * n * 15731 + 789221) + 1376312589) & 0x7fffffff) / 1073741824.0f;
}

static float SmoothNoise(int x, int y, int seed) {
    float corners = (Hash2D(x-1,y-1,seed) + Hash2D(x+1,y-1,seed) +
                     Hash2D(x-1,y+1,seed) + Hash2D(x+1,y+1,seed)) / 16.0f;
    float sides   = (Hash2D(x-1,y,seed) + Hash2D(x+1,y,seed) +
                     Hash2D(x,y-1,seed) + Hash2D(x,y+1,seed)) / 8.0f;
    float center  = Hash2D(x, y, seed) / 4.0f;
    return corners + sides + center;
}

static float CosineInterp(float a, float b, float t) {
    float f = (1.0f - std::cos(t * 3.14159265f)) * 0.5f;
    return a * (1.0f - f) + b * f;
}

static float InterpolatedNoise(float x, float y, int seed) {
    int ix = (int)std::floor(x);
    int iy = (int)std::floor(y);
    float fx = x - ix;
    float fy = y - iy;

    float v1 = SmoothNoise(ix,     iy,     seed);
    float v2 = SmoothNoise(ix + 1, iy,     seed);
    float v3 = SmoothNoise(ix,     iy + 1, seed);
    float v4 = SmoothNoise(ix + 1, iy + 1, seed);

    float i1 = CosineInterp(v1, v2, fx);
    float i2 = CosineInterp(v3, v4, fx);
    return CosineInterp(i1, i2, fy);
}

static float FBMNoise(float x, float y, int octaves, float persistence,
                       float lacunarity, float scale, int seed) {
    float total = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float maxVal = 0.0f;

    for (int i = 0; i < octaves; i++) {
        total += InterpolatedNoise(x * frequency / scale, y * frequency / scale, seed + i) * amplitude;
        maxVal += amplitude;
        amplitude *= persistence;
        frequency *= lacunarity;
    }
    return (total / maxVal + 1.0f) * 0.5f; // normalize to 0-1
}

// ── Terrain implementation ───────────────────────────────────────────

void Terrain::Generate(int resolution, float worldSizeX, float worldSizeZ,
                       float heightScale, const std::vector<float>& heightmap) {
    m_Resolution  = resolution;
    m_WorldSizeX  = worldSizeX;
    m_WorldSizeZ  = worldSizeZ;
    m_HeightScale = heightScale;
    m_Heightmap   = heightmap;

    if ((int)m_Heightmap.size() != resolution * resolution) {
        VE_ENGINE_ERROR("Terrain: heightmap size mismatch ({} vs {}x{})",
                        m_Heightmap.size(), resolution, resolution);
        return;
    }

    BuildMeshFromHeightmap();
}

void Terrain::GenerateProcedural(int resolution, float worldSizeX, float worldSizeZ,
                                  float heightScale, int octaves, float persistence,
                                  float lacunarity, float noiseScale, int seed) {
    m_Resolution  = resolution;
    m_WorldSizeX  = worldSizeX;
    m_WorldSizeZ  = worldSizeZ;
    m_HeightScale = heightScale;
    m_Heightmap.resize(resolution * resolution);

    for (int z = 0; z < resolution; z++) {
        for (int x = 0; x < resolution; x++) {
            float nx = static_cast<float>(x);
            float nz = static_cast<float>(z);
            m_Heightmap[z * resolution + x] = FBMNoise(nx, nz, octaves, persistence,
                                                        lacunarity, noiseScale, seed);
        }
    }

    BuildMeshFromHeightmap();
    VE_ENGINE_INFO("Terrain: procedural {}x{} ({} tris)", resolution, resolution,
                   (resolution - 1) * (resolution - 1) * 2);
}

bool Terrain::GenerateFromImage(const std::string& heightmapPath,
                                 float worldSizeX, float worldSizeZ, float heightScale) {
    int w, h, channels;
    stbi_set_flip_vertically_on_load(0);
    unsigned char* data = stbi_load(heightmapPath.c_str(), &w, &h, &channels, 1);
    if (!data) {
        VE_ENGINE_ERROR("Terrain: failed to load heightmap '{}'", heightmapPath);
        return false;
    }

    m_Resolution  = w; // use image width as resolution (assume square-ish)
    m_WorldSizeX  = worldSizeX;
    m_WorldSizeZ  = worldSizeZ;
    m_HeightScale = heightScale;
    m_Heightmap.resize(w * h);

    for (int i = 0; i < w * h; i++)
        m_Heightmap[i] = data[i] / 255.0f;

    stbi_image_free(data);
    BuildMeshFromHeightmap();
    VE_ENGINE_INFO("Terrain: loaded heightmap '{}' ({}x{}, {} tris)",
                   heightmapPath, w, h, (w - 1) * (h - 1) * 2);
    return true;
}

glm::vec3 Terrain::ComputeNormal(int x, int z) const {
    // Finite differences for smooth normals
    float hL = (x > 0)                ? m_Heightmap[z * m_Resolution + (x - 1)] : m_Heightmap[z * m_Resolution + x];
    float hR = (x < m_Resolution - 1) ? m_Heightmap[z * m_Resolution + (x + 1)] : m_Heightmap[z * m_Resolution + x];
    float hD = (z > 0)                ? m_Heightmap[(z - 1) * m_Resolution + x] : m_Heightmap[z * m_Resolution + x];
    float hU = (z < m_Resolution - 1) ? m_Heightmap[(z + 1) * m_Resolution + x] : m_Heightmap[z * m_Resolution + x];

    float stepX = m_WorldSizeX / (m_Resolution - 1);
    float stepZ = m_WorldSizeZ / (m_Resolution - 1);

    glm::vec3 normal(
        (hL - hR) * m_HeightScale / (2.0f * stepX),
        1.0f,
        (hD - hU) * m_HeightScale / (2.0f * stepZ)
    );
    return glm::normalize(normal);
}

void Terrain::BuildMeshFromHeightmap() {
    if (m_Resolution < 2) return;

    int vertCount = m_Resolution * m_Resolution;
    int quadCount = (m_Resolution - 1) * (m_Resolution - 1);
    int triCount  = quadCount * 2;

    // Vertex layout: pos(3) + normal(3) + color(3) + uv(2) = 11 floats
    std::vector<float> vertices(vertCount * 11);
    std::vector<uint32_t> indices(triCount * 3);

    float stepX = m_WorldSizeX / (m_Resolution - 1);
    float stepZ = m_WorldSizeZ / (m_Resolution - 1);
    float halfX = m_WorldSizeX * 0.5f;
    float halfZ = m_WorldSizeZ * 0.5f;

    // Fill vertices
    for (int z = 0; z < m_Resolution; z++) {
        for (int x = 0; x < m_Resolution; x++) {
            int idx = z * m_Resolution + x;
            float* v = &vertices[idx * 11];

            float h = m_Heightmap[idx] * m_HeightScale;
            float px = x * stepX - halfX;
            float pz = z * stepZ - halfZ;

            // Position
            v[0] = px;
            v[1] = h;
            v[2] = pz;

            // Normal
            glm::vec3 n = ComputeNormal(x, z);
            v[3] = n.x;
            v[4] = n.y;
            v[5] = n.z;

            // Color (white — terrain shader uses textures)
            v[6] = 1.0f;
            v[7] = 1.0f;
            v[8] = 1.0f;

            // UV (0-1 across terrain)
            v[9]  = static_cast<float>(x) / (m_Resolution - 1);
            v[10] = static_cast<float>(z) / (m_Resolution - 1);
        }
    }

    // Fill indices (two triangles per quad)
    int ii = 0;
    for (int z = 0; z < m_Resolution - 1; z++) {
        for (int x = 0; x < m_Resolution - 1; x++) {
            uint32_t tl = z * m_Resolution + x;
            uint32_t tr = tl + 1;
            uint32_t bl = (z + 1) * m_Resolution + x;
            uint32_t br = bl + 1;

            indices[ii++] = tl;
            indices[ii++] = bl;
            indices[ii++] = tr;

            indices[ii++] = tr;
            indices[ii++] = bl;
            indices[ii++] = br;
        }
    }

    // Create GPU resources
    m_VAO = VertexArray::Create();
    auto vb = VertexBuffer::Create(vertices.data(),
        static_cast<uint32_t>(vertices.size() * sizeof(float)));
    vb->SetLayout({
        { ShaderDataType::Float3, "a_Position" },
        { ShaderDataType::Float3, "a_Normal"   },
        { ShaderDataType::Float3, "a_Color"    },
        { ShaderDataType::Float2, "a_TexCoord" },
    });
    m_VAO->AddVertexBuffer(vb);
    m_VAO->SetIndexBuffer(IndexBuffer::Create(indices.data(),
        static_cast<uint32_t>(indices.size())));
}

void Terrain::RebuildMesh() {
    BuildMeshFromHeightmap();
}

float Terrain::GetHeightAt(float worldX, float worldZ) const {
    if (m_Resolution < 2 || m_Heightmap.empty()) return 0.0f;

    float halfX = m_WorldSizeX * 0.5f;
    float halfZ = m_WorldSizeZ * 0.5f;

    // Convert world coords to grid coords
    float gx = (worldX + halfX) / m_WorldSizeX * (m_Resolution - 1);
    float gz = (worldZ + halfZ) / m_WorldSizeZ * (m_Resolution - 1);

    int x0 = std::clamp((int)gx, 0, m_Resolution - 2);
    int z0 = std::clamp((int)gz, 0, m_Resolution - 2);
    float fx = gx - x0;
    float fz = gz - z0;

    float h00 = m_Heightmap[z0 * m_Resolution + x0];
    float h10 = m_Heightmap[z0 * m_Resolution + x0 + 1];
    float h01 = m_Heightmap[(z0 + 1) * m_Resolution + x0];
    float h11 = m_Heightmap[(z0 + 1) * m_Resolution + x0 + 1];

    float h = (h00 * (1 - fx) * (1 - fz) + h10 * fx * (1 - fz) +
               h01 * (1 - fx) * fz + h11 * fx * fz);
    return h * m_HeightScale;
}

} // namespace VE
