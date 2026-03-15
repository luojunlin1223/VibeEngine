/*
 * GLTFImporter.cpp — glTF/GLB mesh import via cgltf
 *
 * Reads all meshes and primitives from a glTF 2.0 / GLB file,
 * extracts vertex attributes (position, normal, texcoord, color),
 * and packs them into the engine's lit vertex layout:
 *   pos(3) + normal(3) + color(3) + uv(2) = 11 floats per vertex
 *
 * Missing normals are computed as flat face normals.
 * Missing UVs default to (0,0). Missing colors default to white (1,1,1).
 */

#include "VibeEngine/Asset/GLTFImporter.h"
#include "VibeEngine/Core/Log.h"

#include <cgltf.h>
#include <glm/glm.hpp>

#include <filesystem>
#include <vector>
#include <unordered_map>
#include <cstring>

namespace VE {

// ── Helpers ──────────────────────────────────────────────────────────────

// Read a float accessor into a flat vector of floats (N components per element)
static std::vector<float> ReadAccessorFloats(const cgltf_accessor* acc) {
    if (!acc) return {};
    cgltf_size numComponents = cgltf_num_components(acc->type);
    cgltf_size totalFloats = acc->count * numComponents;
    std::vector<float> out(totalFloats);
    cgltf_accessor_unpack_floats(acc, out.data(), totalFloats);
    return out;
}

// Read an index accessor into a vector of uint32_t
static std::vector<uint32_t> ReadAccessorIndices(const cgltf_accessor* acc) {
    if (!acc) return {};
    std::vector<uint32_t> out(acc->count);
    for (cgltf_size i = 0; i < acc->count; ++i) {
        out[i] = static_cast<uint32_t>(cgltf_accessor_read_index(acc, i));
    }
    return out;
}

// Find an attribute accessor by type in a primitive
static const cgltf_accessor* FindAttribute(const cgltf_primitive& prim,
                                             cgltf_attribute_type type,
                                             int index = 0) {
    for (cgltf_size i = 0; i < prim.attributes_count; ++i) {
        if (prim.attributes[i].type == type && prim.attributes[i].index == index)
            return prim.attributes[i].data;
    }
    return nullptr;
}

// Compute flat face normals for a triangle list
static void ComputeFaceNormals(std::vector<float>& vertices,
                                const std::vector<uint32_t>& indices,
                                cgltf_size vertexCount) {
    // vertices layout: 11 floats per vertex, normal at offset 3
    // Initialize all normals to zero
    for (cgltf_size i = 0; i < vertexCount; ++i) {
        vertices[i * 11 + 3] = 0.0f;
        vertices[i * 11 + 4] = 0.0f;
        vertices[i * 11 + 5] = 0.0f;
    }

    // Accumulate face normals
    cgltf_size triCount = indices.size() / 3;
    for (cgltf_size t = 0; t < triCount; ++t) {
        uint32_t i0 = indices[t * 3 + 0];
        uint32_t i1 = indices[t * 3 + 1];
        uint32_t i2 = indices[t * 3 + 2];

        glm::vec3 p0(vertices[i0 * 11 + 0], vertices[i0 * 11 + 1], vertices[i0 * 11 + 2]);
        glm::vec3 p1(vertices[i1 * 11 + 0], vertices[i1 * 11 + 1], vertices[i1 * 11 + 2]);
        glm::vec3 p2(vertices[i2 * 11 + 0], vertices[i2 * 11 + 1], vertices[i2 * 11 + 2]);

        glm::vec3 edge1 = p1 - p0;
        glm::vec3 edge2 = p2 - p0;
        glm::vec3 normal = glm::cross(edge1, edge2);
        float len = glm::length(normal);
        if (len > 1e-8f)
            normal /= len;

        for (int k = 0; k < 3; ++k) {
            uint32_t idx = indices[t * 3 + k];
            vertices[idx * 11 + 3] += normal.x;
            vertices[idx * 11 + 4] += normal.y;
            vertices[idx * 11 + 5] += normal.z;
        }
    }

    // Normalize accumulated normals
    for (cgltf_size i = 0; i < vertexCount; ++i) {
        glm::vec3 n(vertices[i * 11 + 3], vertices[i * 11 + 4], vertices[i * 11 + 5]);
        float len = glm::length(n);
        if (len > 1e-8f) {
            n /= len;
        } else {
            n = glm::vec3(0.0f, 1.0f, 0.0f); // fallback up
        }
        vertices[i * 11 + 3] = n.x;
        vertices[i * 11 + 4] = n.y;
        vertices[i * 11 + 5] = n.z;
    }
}

// ── Main import ──────────────────────────────────────────────────────────

std::shared_ptr<MeshAsset> GLTFImporter::Import(const std::string& absPath) {
    cgltf_options options = {};
    cgltf_data* data = nullptr;

    cgltf_result result = cgltf_parse_file(&options, absPath.c_str(), &data);
    if (result != cgltf_result_success) {
        VE_ENGINE_ERROR("GLTFImporter: failed to parse {0} (error {1})", absPath, (int)result);
        return nullptr;
    }

    // Load external buffer data (.bin files for .gltf, embedded for .glb)
    result = cgltf_load_buffers(&options, data, absPath.c_str());
    if (result != cgltf_result_success) {
        VE_ENGINE_ERROR("GLTFImporter: failed to load buffers for {0} (error {1})", absPath, (int)result);
        cgltf_free(data);
        return nullptr;
    }

    if (data->meshes_count == 0) {
        VE_ENGINE_ERROR("GLTFImporter: no meshes in {0}", absPath);
        cgltf_free(data);
        return nullptr;
    }

    // Merged output
    std::vector<float>    allVertices;
    std::vector<uint32_t> allIndices;
    uint32_t baseVertex = 0;
    bool needComputeNormals = false;

    // Iterate all meshes and all primitives
    for (cgltf_size mi = 0; mi < data->meshes_count; ++mi) {
        const cgltf_mesh& mesh = data->meshes[mi];

        for (cgltf_size pi = 0; pi < mesh.primitives_count; ++pi) {
            const cgltf_primitive& prim = mesh.primitives[pi];

            // Only support triangles
            if (prim.type != cgltf_primitive_type_triangles)
                continue;

            // Find required attributes
            const cgltf_accessor* posAcc    = FindAttribute(prim, cgltf_attribute_type_position);
            const cgltf_accessor* normalAcc = FindAttribute(prim, cgltf_attribute_type_normal);
            const cgltf_accessor* uvAcc     = FindAttribute(prim, cgltf_attribute_type_texcoord);
            const cgltf_accessor* colorAcc  = FindAttribute(prim, cgltf_attribute_type_color);

            if (!posAcc) {
                VE_ENGINE_WARN("GLTFImporter: primitive without positions, skipping");
                continue;
            }

            cgltf_size vertexCount = posAcc->count;

            // Read raw attribute data
            std::vector<float> positions = ReadAccessorFloats(posAcc);
            std::vector<float> normals   = normalAcc ? ReadAccessorFloats(normalAcc) : std::vector<float>();
            std::vector<float> uvs       = uvAcc     ? ReadAccessorFloats(uvAcc)     : std::vector<float>();
            std::vector<float> colors    = colorAcc  ? ReadAccessorFloats(colorAcc)  : std::vector<float>();

            bool hasNormals = !normals.empty();
            bool hasUVs     = !uvs.empty();
            bool hasColors  = !colors.empty();
            cgltf_size colorComponents = colorAcc ? cgltf_num_components(colorAcc->type) : 0;

            // Read indices (generate sequential if not indexed)
            std::vector<uint32_t> indices;
            if (prim.indices) {
                indices = ReadAccessorIndices(prim.indices);
            } else {
                indices.resize(vertexCount);
                for (cgltf_size i = 0; i < vertexCount; ++i)
                    indices[i] = static_cast<uint32_t>(i);
            }

            // Pack into interleaved vertex buffer: pos(3)+normal(3)+color(3)+uv(2)
            cgltf_size primBaseVertex = allVertices.size() / 11;
            allVertices.resize(allVertices.size() + vertexCount * 11);

            for (cgltf_size v = 0; v < vertexCount; ++v) {
                float* dst = &allVertices[(primBaseVertex + v) * 11];

                // Position
                dst[0] = positions[v * 3 + 0];
                dst[1] = positions[v * 3 + 1];
                dst[2] = positions[v * 3 + 2];

                // Normal (may be filled later by ComputeFaceNormals)
                if (hasNormals) {
                    dst[3] = normals[v * 3 + 0];
                    dst[4] = normals[v * 3 + 1];
                    dst[5] = normals[v * 3 + 2];
                } else {
                    dst[3] = 0.0f;
                    dst[4] = 1.0f;
                    dst[5] = 0.0f;
                }

                // Color (default white)
                if (hasColors && colorComponents >= 3) {
                    dst[6] = colors[v * colorComponents + 0];
                    dst[7] = colors[v * colorComponents + 1];
                    dst[8] = colors[v * colorComponents + 2];
                } else {
                    dst[6] = 1.0f;
                    dst[7] = 1.0f;
                    dst[8] = 1.0f;
                }

                // UV (default zero)
                if (hasUVs) {
                    dst[9]  = uvs[v * 2 + 0];
                    dst[10] = uvs[v * 2 + 1];
                } else {
                    dst[9]  = 0.0f;
                    dst[10] = 0.0f;
                }
            }

            // Offset indices and append
            for (auto idx : indices)
                allIndices.push_back(static_cast<uint32_t>(primBaseVertex) + idx);

            if (!hasNormals)
                needComputeNormals = true;
        }
    }

    cgltf_free(data);

    if (allVertices.empty()) {
        VE_ENGINE_ERROR("GLTFImporter: no valid triangle primitives in {0}", absPath);
        return nullptr;
    }

    // Compute face normals if any primitives lacked them
    if (needComputeNormals) {
        ComputeFaceNormals(allVertices, allIndices, allVertices.size() / 11);
    }

    // Build MeshAsset
    auto asset = std::make_shared<MeshAsset>();
    asset->SourcePath = absPath;
    asset->SetName(std::filesystem::path(absPath).stem().string());
    asset->Vertices = std::move(allVertices);
    asset->Indices  = std::move(allIndices);
    asset->ComputeBoundingBox();
    asset->Upload();

    uint32_t vertCount = static_cast<uint32_t>(asset->Vertices.size() / 11);
    uint32_t triCount  = static_cast<uint32_t>(asset->Indices.size() / 3);
    VE_ENGINE_INFO("GLTFImporter: {0} — {1} verts, {2} tris",
                   std::filesystem::path(absPath).filename().string(),
                   vertCount, triCount);

    return asset;
}

} // namespace VE
