#include "VibeEngine/Asset/MeshImporter.h"
#include "VibeEngine/Core/Log.h"

#include <ufbx.h>
#include <unordered_map>
#include <functional>
#include <filesystem>

namespace VE {

std::unordered_map<std::string, std::shared_ptr<MeshAsset>> MeshImporter::s_Cache;

// Vertex key for deduplication
struct VertexKey {
    float pos[3], nrm[3], uv[2];
    bool operator==(const VertexKey& o) const {
        return std::memcmp(this, &o, sizeof(VertexKey)) == 0;
    }
};

struct VertexKeyHash {
    size_t operator()(const VertexKey& k) const {
        size_t h = 0;
        const auto* data = reinterpret_cast<const uint32_t*>(&k);
        for (size_t i = 0; i < sizeof(VertexKey) / 4; i++)
            h ^= std::hash<uint32_t>()(data[i]) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

std::shared_ptr<MeshAsset> MeshImporter::LoadFBX(const std::string& absolutePath) {
    ufbx_load_opts opts = {};
    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.target_unit_meters = 1.0f;

    ufbx_error error;
    ufbx_scene* scene = ufbx_load_file(absolutePath.c_str(), &opts, &error);
    if (!scene) {
        VE_ENGINE_ERROR("Failed to load FBX: {0} - {1}", absolutePath, error.description.data);
        return nullptr;
    }

    if (scene->meshes.count == 0) {
        VE_ENGINE_ERROR("FBX file has no meshes: {0}", absolutePath);
        ufbx_free_scene(scene);
        return nullptr;
    }

    auto asset = std::make_shared<MeshAsset>();
    asset->Name = std::filesystem::path(absolutePath).stem().generic_string();
    asset->SourcePath = absolutePath;

    // Merge all meshes
    std::unordered_map<VertexKey, uint32_t, VertexKeyHash> vertexMap;

    for (size_t mi = 0; mi < scene->meshes.count; mi++) {
        ufbx_mesh* mesh = scene->meshes.data[mi];

        // Triangulate faces
        size_t maxTriIndices = mesh->max_face_triangles * 3;
        std::vector<uint32_t> triIndices(maxTriIndices);

        for (size_t fi = 0; fi < mesh->faces.count; fi++) {
            ufbx_face face = mesh->faces.data[fi];
            uint32_t numTris = ufbx_triangulate_face(triIndices.data(), maxTriIndices, mesh, face);

            for (uint32_t ti = 0; ti < numTris * 3; ti++) {
                uint32_t idx = triIndices[ti];

                ufbx_vec3 pos = ufbx_get_vertex_vec3(&mesh->vertex_position, idx);
                ufbx_vec3 nrm = { 0, 1, 0 };
                if (mesh->vertex_normal.exists)
                    nrm = ufbx_get_vertex_vec3(&mesh->vertex_normal, idx);

                ufbx_vec2 uv = { 0, 0 };
                if (mesh->vertex_uv.exists)
                    uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, idx);

                VertexKey key;
                key.pos[0] = (float)pos.x; key.pos[1] = (float)pos.y; key.pos[2] = (float)pos.z;
                key.nrm[0] = (float)nrm.x; key.nrm[1] = (float)nrm.y; key.nrm[2] = (float)nrm.z;
                key.uv[0]  = (float)uv.x;  key.uv[1]  = (float)uv.y;

                auto it = vertexMap.find(key);
                if (it != vertexMap.end()) {
                    asset->Indices.push_back(it->second);
                } else {
                    uint32_t newIdx = static_cast<uint32_t>(asset->Vertices.size() / 11);
                    // pos(3) + normal(3) + color(3) + uv(2)
                    asset->Vertices.push_back(key.pos[0]);
                    asset->Vertices.push_back(key.pos[1]);
                    asset->Vertices.push_back(key.pos[2]);
                    asset->Vertices.push_back(key.nrm[0]);
                    asset->Vertices.push_back(key.nrm[1]);
                    asset->Vertices.push_back(key.nrm[2]);
                    asset->Vertices.push_back(1.0f); // vertex color R
                    asset->Vertices.push_back(1.0f); // vertex color G
                    asset->Vertices.push_back(1.0f); // vertex color B
                    asset->Vertices.push_back(key.uv[0]);
                    asset->Vertices.push_back(key.uv[1]);
                    vertexMap[key] = newIdx;
                    asset->Indices.push_back(newIdx);
                }
            }
        }
    }

    ufbx_free_scene(scene);

    asset->Upload();
    VE_ENGINE_INFO("FBX loaded: {0} ({1} meshes merged, {2} verts, {3} tris)",
        absolutePath, scene == nullptr ? 0 : 0, // already freed
        asset->Vertices.size() / 11, asset->Indices.size() / 3);

    return asset;
}

std::shared_ptr<MeshAsset> MeshImporter::GetOrLoad(const std::string& absolutePath) {
    auto it = s_Cache.find(absolutePath);
    if (it != s_Cache.end() && it->second && it->second->VAO)
        return it->second;

    auto asset = LoadFBX(absolutePath);
    if (asset)
        s_Cache[absolutePath] = asset;
    return asset;
}

void MeshImporter::ClearCache() {
    for (auto& [path, asset] : s_Cache) {
        if (asset) asset->Release();
    }
    // Don't erase - keep CPU data for re-upload
}

void MeshImporter::ReuploadCache() {
    for (auto& [path, asset] : s_Cache) {
        if (asset && !asset->VAO)
            asset->Upload();
    }
}

} // namespace VE
