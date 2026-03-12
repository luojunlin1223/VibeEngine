#include "VibeEngine/Asset/FBXImporter.h"
#include "VibeEngine/Core/Log.h"

#include <yaml-cpp/yaml.h>
#include <ufbx.h>

#include <fstream>
#include <filesystem>
#include <unordered_map>
#include <functional>
#include <cstring>
#include <cmath>

namespace VE {

// ── Settings persistence ────────────────────────────────────────────

FBXImportSettings FBXImporter::LoadSettings(const std::string& metaPath) {
    FBXImportSettings s;
    if (!std::filesystem::exists(metaPath)) return s;

    try {
        YAML::Node root = YAML::LoadFile(metaPath);
        YAML::Node is = root["importSettings"];
        if (!is) return s;

        if (is["scaleFactor"])        s.ScaleFactor        = is["scaleFactor"].as<float>();
        if (is["normals"]) {
            std::string n = is["normals"].as<std::string>();
            if (n == "Calculate")     s.Normals = FBXImportSettings::NormalMode::Calculate;
            else if (n == "None")     s.Normals = FBXImportSettings::NormalMode::None;
            else                      s.Normals = FBXImportSettings::NormalMode::Import;
        }
        if (is["importUVs"])          s.ImportUVs          = is["importUVs"].as<bool>();
        if (is["mergeAllMeshes"])     s.MergeAllMeshes     = is["mergeAllMeshes"].as<bool>();
        if (is["importVertexColors"]) s.ImportVertexColors  = is["importVertexColors"].as<bool>();

        // Read cached mesh info
        if (is["vertexCount"])   s.VertexCount   = is["vertexCount"].as<uint32_t>();
        if (is["triangleCount"]) s.TriangleCount = is["triangleCount"].as<uint32_t>();
        if (is["subMeshCount"])  s.SubMeshCount  = is["subMeshCount"].as<uint32_t>();
    } catch (const std::exception& e) {
        VE_ENGINE_WARN("FBXImporter: failed to parse import settings from {0}: {1}", metaPath, e.what());
    }
    return s;
}

void FBXImporter::SaveSettings(const std::string& metaPath, const FBXImportSettings& settings) {
    // Read existing meta to preserve uuid/type
    uint64_t uuid = 0;
    std::string type = "Mesh";
    if (std::filesystem::exists(metaPath)) {
        try {
            YAML::Node existing = YAML::LoadFile(metaPath);
            if (existing["uuid"]) uuid = existing["uuid"].as<uint64_t>();
            if (existing["type"]) type = existing["type"].as<std::string>();
        } catch (...) {}
    }

    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "uuid" << YAML::Value << uuid;
    out << YAML::Key << "type" << YAML::Value << type;

    out << YAML::Key << "importSettings" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "scaleFactor"        << YAML::Value << settings.ScaleFactor;
    const char* normalStr = "Import";
    if (settings.Normals == FBXImportSettings::NormalMode::Calculate) normalStr = "Calculate";
    else if (settings.Normals == FBXImportSettings::NormalMode::None) normalStr = "None";
    out << YAML::Key << "normals"            << YAML::Value << normalStr;
    out << YAML::Key << "importUVs"          << YAML::Value << settings.ImportUVs;
    out << YAML::Key << "mergeAllMeshes"     << YAML::Value << settings.MergeAllMeshes;
    out << YAML::Key << "importVertexColors" << YAML::Value << settings.ImportVertexColors;
    out << YAML::Key << "vertexCount"        << YAML::Value << settings.VertexCount;
    out << YAML::Key << "triangleCount"      << YAML::Value << settings.TriangleCount;
    out << YAML::Key << "subMeshCount"       << YAML::Value << settings.SubMeshCount;
    out << YAML::EndMap; // importSettings

    out << YAML::EndMap;

    std::ofstream fout(metaPath);
    fout << out.c_str();
}

// ── Vertex deduplication helpers ────────────────────────────────────

struct VertexKey {
    float pos[3], nrm[3], col[3], uv[2];
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

// ── Simple flat-shading normal calculation ──────────────────────────

static void CalculateFlatNormals(std::vector<float>& vertices, const std::vector<uint32_t>& indices) {
    // Each triangle gets a flat normal; vertices are interleaved with stride 11
    const int stride = 11;
    // Zero out all normals first
    for (size_t i = 0; i < vertices.size() / stride; i++) {
        vertices[i * stride + 3] = 0.0f;
        vertices[i * stride + 4] = 0.0f;
        vertices[i * stride + 5] = 0.0f;
    }
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        uint32_t i0 = indices[i], i1 = indices[i + 1], i2 = indices[i + 2];
        float* p0 = &vertices[i0 * stride];
        float* p1 = &vertices[i1 * stride];
        float* p2 = &vertices[i2 * stride];
        // edge vectors
        float e1x = p1[0] - p0[0], e1y = p1[1] - p0[1], e1z = p1[2] - p0[2];
        float e2x = p2[0] - p0[0], e2y = p2[1] - p0[1], e2z = p2[2] - p0[2];
        // cross product
        float nx = e1y * e2z - e1z * e2y;
        float ny = e1z * e2x - e1x * e2z;
        float nz = e1x * e2y - e1y * e2x;
        float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 1e-8f) { nx /= len; ny /= len; nz /= len; }
        // Assign same normal to all 3 verts (flat shading — accumulate for smooth)
        for (uint32_t vi : {i0, i1, i2}) {
            vertices[vi * stride + 3] += nx;
            vertices[vi * stride + 4] += ny;
            vertices[vi * stride + 5] += nz;
        }
    }
    // Normalize accumulated normals
    for (size_t i = 0; i < vertices.size() / stride; i++) {
        float* n = &vertices[i * stride + 3];
        float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len > 1e-8f) { n[0] /= len; n[1] /= len; n[2] /= len; }
        else { n[0] = 0; n[1] = 1; n[2] = 0; }
    }
}

// ── Import ──────────────────────────────────────────────────────────

std::shared_ptr<MeshAsset> FBXImporter::Import(const std::string& absPath,
                                                 FBXImportSettings& settings) {
    ufbx_load_opts opts = {};
    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.target_unit_meters = 1.0f;

    ufbx_error error;
    ufbx_scene* scene = ufbx_load_file(absPath.c_str(), &opts, &error);
    if (!scene) {
        VE_ENGINE_ERROR("FBXImporter: failed to load {0} - {1}", absPath, error.description.data);
        return nullptr;
    }

    if (scene->meshes.count == 0) {
        VE_ENGINE_ERROR("FBXImporter: no meshes in {0}", absPath);
        ufbx_free_scene(scene);
        return nullptr;
    }

    settings.SubMeshCount = static_cast<uint32_t>(scene->meshes.count);

    auto asset = std::make_shared<MeshAsset>();
    asset->SetName(std::filesystem::path(absPath).stem().generic_string());
    asset->SourcePath = absPath;

    std::unordered_map<VertexKey, uint32_t, VertexKeyHash> vertexMap;

    size_t meshCount = settings.MergeAllMeshes ? scene->meshes.count : std::min<size_t>(1, scene->meshes.count);
    for (size_t mi = 0; mi < meshCount; mi++) {
        ufbx_mesh* mesh = scene->meshes.data[mi];

        size_t maxTriIndices = mesh->max_face_triangles * 3;
        std::vector<uint32_t> triIndices(maxTriIndices);

        for (size_t fi = 0; fi < mesh->faces.count; fi++) {
            ufbx_face face = mesh->faces.data[fi];
            uint32_t numTris = ufbx_triangulate_face(triIndices.data(), maxTriIndices, mesh, face);

            for (uint32_t ti = 0; ti < numTris * 3; ti++) {
                uint32_t idx = triIndices[ti];

                ufbx_vec3 pos = ufbx_get_vertex_vec3(&mesh->vertex_position, idx);

                // Apply scale factor
                pos.x *= settings.ScaleFactor;
                pos.y *= settings.ScaleFactor;
                pos.z *= settings.ScaleFactor;

                ufbx_vec3 nrm = {0, 1, 0};
                if (settings.Normals == FBXImportSettings::NormalMode::Import && mesh->vertex_normal.exists)
                    nrm = ufbx_get_vertex_vec3(&mesh->vertex_normal, idx);
                // NormalMode::Calculate handled after vertex collection
                // NormalMode::None leaves default (0,1,0)

                ufbx_vec2 uv = {0, 0};
                if (settings.ImportUVs && mesh->vertex_uv.exists)
                    uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, idx);

                float cr = 1.0f, cg = 1.0f, cb = 1.0f;
                if (settings.ImportVertexColors && mesh->vertex_color.exists) {
                    ufbx_vec4 col = ufbx_get_vertex_vec4(&mesh->vertex_color, idx);
                    cr = (float)col.x; cg = (float)col.y; cb = (float)col.z;
                }

                VertexKey key;
                key.pos[0] = (float)pos.x; key.pos[1] = (float)pos.y; key.pos[2] = (float)pos.z;
                key.nrm[0] = (float)nrm.x; key.nrm[1] = (float)nrm.y; key.nrm[2] = (float)nrm.z;
                key.col[0] = cr; key.col[1] = cg; key.col[2] = cb;
                key.uv[0]  = (float)uv.x;  key.uv[1]  = (float)uv.y;

                auto it = vertexMap.find(key);
                if (it != vertexMap.end()) {
                    asset->Indices.push_back(it->second);
                } else {
                    uint32_t newIdx = static_cast<uint32_t>(asset->Vertices.size() / 11);
                    asset->Vertices.push_back(key.pos[0]);
                    asset->Vertices.push_back(key.pos[1]);
                    asset->Vertices.push_back(key.pos[2]);
                    asset->Vertices.push_back(key.nrm[0]);
                    asset->Vertices.push_back(key.nrm[1]);
                    asset->Vertices.push_back(key.nrm[2]);
                    asset->Vertices.push_back(key.col[0]);
                    asset->Vertices.push_back(key.col[1]);
                    asset->Vertices.push_back(key.col[2]);
                    asset->Vertices.push_back(key.uv[0]);
                    asset->Vertices.push_back(key.uv[1]);
                    vertexMap[key] = newIdx;
                    asset->Indices.push_back(newIdx);
                }
            }
        }
    }

    ufbx_free_scene(scene);

    // Calculate normals if requested
    if (settings.Normals == FBXImportSettings::NormalMode::Calculate)
        CalculateFlatNormals(asset->Vertices, asset->Indices);

    // Populate info fields
    settings.VertexCount   = static_cast<uint32_t>(asset->Vertices.size() / 11);
    settings.TriangleCount = static_cast<uint32_t>(asset->Indices.size() / 3);

    asset->Upload();
    VE_ENGINE_INFO("FBXImporter: {0} — {1} verts, {2} tris, scale={3}",
        absPath, settings.VertexCount, settings.TriangleCount, settings.ScaleFactor);

    return asset;
}

} // namespace VE
