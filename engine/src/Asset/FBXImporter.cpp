#include "VibeEngine/Asset/FBXImporter.h"
#include "VibeEngine/Animation/Skeleton.h"
#include "VibeEngine/Animation/AnimationClip.h"
#include "VibeEngine/Core/Log.h"

#include <yaml-cpp/yaml.h>
#include <ufbx.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <fstream>
#include <filesystem>
#include <unordered_map>
#include <functional>
#include <cstring>
#include <cmath>
#include <algorithm>

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
        if (is["importSkinWeights"]) s.ImportSkinWeights  = is["importSkinWeights"].as<bool>();
        if (is["importAnimations"])  s.ImportAnimations   = is["importAnimations"].as<bool>();

        // Read cached mesh info
        if (is["vertexCount"])   s.VertexCount   = is["vertexCount"].as<uint32_t>();
        if (is["triangleCount"]) s.TriangleCount = is["triangleCount"].as<uint32_t>();
        if (is["subMeshCount"])  s.SubMeshCount  = is["subMeshCount"].as<uint32_t>();
        if (is["boneCount"])     s.BoneCount     = is["boneCount"].as<uint32_t>();
        if (is["clipCount"])     s.ClipCount     = is["clipCount"].as<uint32_t>();
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
    out << YAML::Key << "importSkinWeights"  << YAML::Value << settings.ImportSkinWeights;
    out << YAML::Key << "importAnimations"   << YAML::Value << settings.ImportAnimations;
    out << YAML::Key << "vertexCount"        << YAML::Value << settings.VertexCount;
    out << YAML::Key << "triangleCount"      << YAML::Value << settings.TriangleCount;
    out << YAML::Key << "subMeshCount"       << YAML::Value << settings.SubMeshCount;
    out << YAML::Key << "boneCount"          << YAML::Value << settings.BoneCount;
    out << YAML::Key << "clipCount"          << YAML::Value << settings.ClipCount;
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

    // Track mapping: ufbx mesh-local vertex index → deduplicated output vertex index
    // We use (meshIndex, ufbx_vertex_index) → output_vertex_index
    // For skinning we need to map from ufbx control point index to output vertex indices
    // ufbx vertex_position.indices[idx] gives the control point index for face-corner idx
    // We accumulate: for each mesh, controlPointIndex → list of output vertex indices
    struct MeshSkinInfo {
        ufbx_mesh* mesh;
        std::unordered_map<uint32_t, std::vector<uint32_t>> cpToOutputVerts;
    };
    std::vector<MeshSkinInfo> meshSkinInfos;

    size_t meshCount = settings.MergeAllMeshes ? scene->meshes.count : std::min<size_t>(1, scene->meshes.count);
    for (size_t mi = 0; mi < meshCount; mi++) {
        ufbx_mesh* mesh = scene->meshes.data[mi];
        MeshSkinInfo skinInfo;
        skinInfo.mesh = mesh;

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
                uint32_t outputIdx;
                if (it != vertexMap.end()) {
                    outputIdx = it->second;
                    asset->Indices.push_back(outputIdx);
                } else {
                    outputIdx = static_cast<uint32_t>(asset->Vertices.size() / 11);
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
                    vertexMap[key] = outputIdx;
                    asset->Indices.push_back(outputIdx);
                }

                // Track control point → output vertex mapping for skinning
                uint32_t cpIdx = mesh->vertex_position.indices.data[idx];
                skinInfo.cpToOutputVerts[cpIdx].push_back(outputIdx);
            }
        }
        meshSkinInfos.push_back(std::move(skinInfo));
    }

    // Calculate normals if requested
    if (settings.Normals == FBXImportSettings::NormalMode::Calculate)
        CalculateFlatNormals(asset->Vertices, asset->Indices);

    uint32_t totalVerts = static_cast<uint32_t>(asset->Vertices.size() / 11);

    // ── Skeleton & Skin Weights extraction ──────────────────────────
    bool hasSkin = false;
    if (settings.ImportSkinWeights) {
        for (auto& msi : meshSkinInfos) {
            if (msi.mesh->skin_deformers.count > 0) {
                hasSkin = true;
                break;
            }
        }
    }

    if (hasSkin) {
        auto skeleton = std::make_shared<Skeleton>();

        // Build bone list from all skin clusters across all meshes
        // Map bone_node → bone index
        std::unordered_map<ufbx_node*, int> boneNodeMap;

        for (auto& msi : meshSkinInfos) {
            for (size_t di = 0; di < msi.mesh->skin_deformers.count; di++) {
                ufbx_skin_deformer* skin = msi.mesh->skin_deformers.data[di];
                for (size_t ci = 0; ci < skin->clusters.count; ci++) {
                    ufbx_skin_cluster* cluster = skin->clusters.data[ci];
                    ufbx_node* boneNode = cluster->bone_node;
                    if (!boneNode) continue;
                    if (boneNodeMap.count(boneNode)) continue;

                    int boneIdx = static_cast<int>(skeleton->Bones.size());
                    boneNodeMap[boneNode] = boneIdx;

                    Bone bone;
                    bone.Name = boneNode->name.data;
                    bone.ParentIndex = -1; // resolved below

                    // Inverse bind matrix from geometry_to_bone
                    auto& m = cluster->geometry_to_bone;
                    bone.InverseBindMatrix = glm::mat4(
                        (float)m.m00, (float)m.m10, (float)m.m20, 0.0f,
                        (float)m.m01, (float)m.m11, (float)m.m21, 0.0f,
                        (float)m.m02, (float)m.m12, (float)m.m22, 0.0f,
                        (float)m.m03, (float)m.m13, (float)m.m23, 1.0f
                    );

                    // Local bind transform from node
                    ufbx_transform lt = boneNode->local_transform;
                    glm::vec3 pos((float)lt.translation.x, (float)lt.translation.y, (float)lt.translation.z);
                    glm::quat rot((float)lt.rotation.w, (float)lt.rotation.x, (float)lt.rotation.y, (float)lt.rotation.z);
                    glm::vec3 scl((float)lt.scale.x, (float)lt.scale.y, (float)lt.scale.z);
                    glm::mat4 T = glm::translate(glm::mat4(1.0f), pos);
                    glm::mat4 R = glm::mat4_cast(rot);
                    glm::mat4 S = glm::scale(glm::mat4(1.0f), scl);
                    bone.LocalBindTransform = T * R * S;

                    skeleton->Bones.push_back(bone);
                }
            }
        }

        // Resolve parent indices
        for (auto& [node, idx] : boneNodeMap) {
            if (node->parent) {
                auto parentIt = boneNodeMap.find(node->parent);
                if (parentIt != boneNodeMap.end())
                    skeleton->Bones[idx].ParentIndex = parentIt->second;
            }
        }

        asset->SkeletonRef = skeleton;

        // Extract per-vertex skin weights
        asset->SkinData.resize(totalVerts);

        for (auto& msi : meshSkinInfos) {
            for (size_t di = 0; di < msi.mesh->skin_deformers.count; di++) {
                ufbx_skin_deformer* skin = msi.mesh->skin_deformers.data[di];
                for (size_t ci = 0; ci < skin->clusters.count; ci++) {
                    ufbx_skin_cluster* cluster = skin->clusters.data[ci];
                    if (!cluster->bone_node) continue;
                    int boneIdx = boneNodeMap[cluster->bone_node];

                    for (size_t vi = 0; vi < cluster->vertices.count; vi++) {
                        uint32_t cpIdx = static_cast<uint32_t>(cluster->vertices.data[vi]);
                        float weight = (float)cluster->weights.data[vi];

                        auto cpIt = msi.cpToOutputVerts.find(cpIdx);
                        if (cpIt == msi.cpToOutputVerts.end()) continue;

                        for (uint32_t outVert : cpIt->second) {
                            auto& sv = asset->SkinData[outVert];
                            // Insert into the slot with the smallest weight
                            int minSlot = 0;
                            for (int s = 1; s < 4; s++) {
                                if (sv.BoneWeights[s] < sv.BoneWeights[minSlot])
                                    minSlot = s;
                            }
                            if (weight > sv.BoneWeights[minSlot]) {
                                sv.BoneIndices[minSlot] = boneIdx;
                                sv.BoneWeights[minSlot] = weight;
                            }
                        }
                    }
                }
            }
        }

        // Normalize weights
        for (auto& sv : asset->SkinData) {
            float total = sv.BoneWeights[0] + sv.BoneWeights[1] + sv.BoneWeights[2] + sv.BoneWeights[3];
            if (total > 1e-6f) {
                for (int i = 0; i < 4; i++)
                    sv.BoneWeights[i] /= total;
            }
        }

        settings.BoneCount = static_cast<uint32_t>(skeleton->Bones.size());

        VE_ENGINE_INFO("FBXImporter: skeleton — {0} bones", settings.BoneCount);

        // ── Animation clip extraction via ufbx_bake_anim ─────────────
        if (settings.ImportAnimations) {
            // Try per-stack first, then fall back to scene->anim (global)
            struct AnimSource {
                const char* name;
                ufbx_anim* anim;
            };
            std::vector<AnimSource> sources;
            for (size_t ai = 0; ai < scene->anim_stacks.count; ai++)
                sources.push_back({ scene->anim_stacks.data[ai]->name.data, scene->anim_stacks.data[ai]->anim });

            // Also try the global scene animation as a fallback
            if (scene->anim)
                sources.push_back({ "SceneAnim", scene->anim });

            for (auto& src : sources) {
                ufbx_bake_opts bakeOpts = {};
                ufbx_error bakeError;
                ufbx_baked_anim* baked = ufbx_bake_anim(scene, src.anim, &bakeOpts, &bakeError);
                if (!baked) {
                    VE_ENGINE_WARN("FBXImporter: failed to bake anim '{0}'", src.name);
                    continue;
                }

                // Use actual keyframe time range
                double keyMin = baked->key_time_min;
                double keyMax = baked->key_time_max;
                double keyRange = keyMax - keyMin;
                double playRange = baked->playback_duration;

                VE_ENGINE_INFO("FBXImporter: baking '{0}' keyRange={1:.3f}s, playback={2:.3f}s, nodes={3}",
                    src.name, keyRange, playRange, baked->nodes.count);

                // Skip if no useful data
                if (baked->nodes.count == 0 || keyRange < 0.05) {
                    ufbx_free_baked_anim(baked);
                    continue;
                }

                AnimationClip clip;
                clip.Name = src.name;
                clip.Duration = (float)keyRange;

                // Extract baked keyframes per bone
                for (auto& [node, boneIdx] : boneNodeMap) {
                    ufbx_baked_node* bakedNode = ufbx_find_baked_node(baked, node);
                    if (!bakedNode) continue;

                    BoneTrack track;
                    track.BoneIndex = boneIdx;

                    // Collect all unique time stamps from T/R/S channels
                    std::vector<double> times;
                    for (size_t i = 0; i < bakedNode->translation_keys.count; i++)
                        times.push_back(bakedNode->translation_keys.data[i].time);
                    for (size_t i = 0; i < bakedNode->rotation_keys.count; i++)
                        times.push_back(bakedNode->rotation_keys.data[i].time);
                    for (size_t i = 0; i < bakedNode->scale_keys.count; i++)
                        times.push_back(bakedNode->scale_keys.data[i].time);

                    std::sort(times.begin(), times.end());
                    times.erase(std::unique(times.begin(), times.end(),
                        [](double a, double b) { return std::abs(a - b) < 1e-7; }), times.end());

                    // For each unique time, sample T/R/S by lerping the baked keys
                    auto sampleVec3 = [](const ufbx_baked_vec3_list& keys, double t) -> glm::vec3 {
                        if (keys.count == 0) return glm::vec3(0.0f);
                        if (keys.count == 1 || t <= keys.data[0].time)
                            return glm::vec3((float)keys.data[0].value.x, (float)keys.data[0].value.y, (float)keys.data[0].value.z);
                        for (size_t i = 0; i + 1 < keys.count; i++) {
                            if (t <= keys.data[i + 1].time) {
                                double seg = keys.data[i + 1].time - keys.data[i].time;
                                float frac = (seg > 1e-9) ? (float)((t - keys.data[i].time) / seg) : 0.0f;
                                auto& a = keys.data[i].value;
                                auto& b = keys.data[i + 1].value;
                                return glm::vec3(
                                    glm::mix((float)a.x, (float)b.x, frac),
                                    glm::mix((float)a.y, (float)b.y, frac),
                                    glm::mix((float)a.z, (float)b.z, frac));
                            }
                        }
                        auto& last = keys.data[keys.count - 1].value;
                        return glm::vec3((float)last.x, (float)last.y, (float)last.z);
                    };

                    auto sampleQuat = [](const ufbx_baked_quat_list& keys, double t) -> glm::quat {
                        if (keys.count == 0) return glm::quat(1, 0, 0, 0);
                        if (keys.count == 1 || t <= keys.data[0].time) {
                            auto& q = keys.data[0].value;
                            return glm::quat((float)q.w, (float)q.x, (float)q.y, (float)q.z);
                        }
                        for (size_t i = 0; i + 1 < keys.count; i++) {
                            if (t <= keys.data[i + 1].time) {
                                double seg = keys.data[i + 1].time - keys.data[i].time;
                                float frac = (seg > 1e-9) ? (float)((t - keys.data[i].time) / seg) : 0.0f;
                                auto& a = keys.data[i].value;
                                auto& b = keys.data[i + 1].value;
                                glm::quat qa((float)a.w, (float)a.x, (float)a.y, (float)a.z);
                                glm::quat qb((float)b.w, (float)b.x, (float)b.y, (float)b.z);
                                return glm::slerp(qa, qb, frac);
                            }
                        }
                        auto& q = keys.data[keys.count - 1].value;
                        return glm::quat((float)q.w, (float)q.x, (float)q.y, (float)q.z);
                    };

                    for (double t : times) {
                        BoneKeyframe kf;
                        kf.Time = (float)(t - keyMin); // offset to start from 0
                        kf.Position = sampleVec3(bakedNode->translation_keys, t);
                        kf.Rotation = sampleQuat(bakedNode->rotation_keys, t);
                        kf.Scale = sampleVec3(bakedNode->scale_keys, t);
                        // Default scale to 1 if no keys
                        if (bakedNode->scale_keys.count == 0)
                            kf.Scale = glm::vec3(1.0f);
                        track.Keyframes.push_back(kf);
                    }

                    if (!track.Keyframes.empty())
                        clip.Tracks.push_back(std::move(track));
                }

                ufbx_free_baked_anim(baked);
                asset->Clips.push_back(std::move(clip));
            }

            settings.ClipCount = static_cast<uint32_t>(asset->Clips.size());
            VE_ENGINE_INFO("FBXImporter: {0} animation clip(s)", settings.ClipCount);
        }
    }

    ufbx_free_scene(scene);

    // Copy vertices → bind pose before any modifications
    asset->BindPoseVertices = asset->Vertices;

    // Populate info fields
    settings.VertexCount   = totalVerts;
    settings.TriangleCount = static_cast<uint32_t>(asset->Indices.size() / 3);

    asset->Upload();
    VE_ENGINE_INFO("FBXImporter: {0} — {1} verts, {2} tris, scale={3}",
        absPath, settings.VertexCount, settings.TriangleCount, settings.ScaleFactor);

    return asset;
}

// ── Import animations only (from external FBX, remapped to existing skeleton) ──

std::vector<AnimationClip> FBXImporter::ImportAnimations(const std::string& absPath,
                                                          const std::shared_ptr<Skeleton>& skeleton) {
    std::vector<AnimationClip> result;
    if (!skeleton || skeleton->Bones.empty()) return result;

    ufbx_load_opts opts = {};
    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.target_unit_meters = 1.0f;

    ufbx_error error;
    ufbx_scene* scene = ufbx_load_file(absPath.c_str(), &opts, &error);
    if (!scene) {
        VE_ENGINE_ERROR("FBXImporter::ImportAnimations: failed to load {0} - {1}", absPath, error.description.data);
        return result;
    }

    // Build a map: node name → ufbx_node* for all nodes in this scene
    std::unordered_map<std::string, ufbx_node*> nodeByName;
    for (size_t i = 0; i < scene->nodes.count; i++) {
        ufbx_node* node = scene->nodes.data[i];
        nodeByName[node->name.data] = node;
    }

    // Collect all anim sources (per-stack + global)
    struct AnimSource { const char* name; ufbx_anim* anim; };
    std::vector<AnimSource> sources;
    for (size_t ai = 0; ai < scene->anim_stacks.count; ai++)
        sources.push_back({ scene->anim_stacks.data[ai]->name.data, scene->anim_stacks.data[ai]->anim });
    if (scene->anim)
        sources.push_back({ "SceneAnim", scene->anim });

    for (auto& src : sources) {
        ufbx_bake_opts bakeOpts = {};
        ufbx_error bakeError;
        ufbx_baked_anim* baked = ufbx_bake_anim(scene, src.anim, &bakeOpts, &bakeError);
        if (!baked) continue;

        double keyMin = baked->key_time_min;
        double keyMax = baked->key_time_max;
        double keyRange = keyMax - keyMin;

        VE_ENGINE_INFO("FBXImporter::ImportAnimations: '{0}' keyRange={1:.3f}s, nodes={2}",
            src.name, keyRange, baked->nodes.count);

        if (baked->nodes.count == 0 || keyRange < 0.05) {
            ufbx_free_baked_anim(baked);
            continue;
        }

        AnimationClip clip;
        clip.Name = src.name;
        clip.Duration = (float)keyRange;

        // For each bone in the target skeleton, find matching node in this FBX by name
        for (int bi = 0; bi < skeleton->GetBoneCount(); bi++) {
            auto& bone = skeleton->Bones[bi];
            auto nodeIt = nodeByName.find(bone.Name);
            if (nodeIt == nodeByName.end()) continue;

            ufbx_baked_node* bakedNode = ufbx_find_baked_node(baked, nodeIt->second);
            if (!bakedNode) continue;

            BoneTrack track;
            track.BoneIndex = bi;

            // Collect unique timestamps
            std::vector<double> times;
            for (size_t i = 0; i < bakedNode->translation_keys.count; i++)
                times.push_back(bakedNode->translation_keys.data[i].time);
            for (size_t i = 0; i < bakedNode->rotation_keys.count; i++)
                times.push_back(bakedNode->rotation_keys.data[i].time);
            for (size_t i = 0; i < bakedNode->scale_keys.count; i++)
                times.push_back(bakedNode->scale_keys.data[i].time);

            std::sort(times.begin(), times.end());
            times.erase(std::unique(times.begin(), times.end(),
                [](double a, double b) { return std::abs(a - b) < 1e-7; }), times.end());

            auto sampleVec3 = [](const ufbx_baked_vec3_list& keys, double t) -> glm::vec3 {
                if (keys.count == 0) return glm::vec3(0.0f);
                if (keys.count == 1 || t <= keys.data[0].time)
                    return glm::vec3((float)keys.data[0].value.x, (float)keys.data[0].value.y, (float)keys.data[0].value.z);
                for (size_t i = 0; i + 1 < keys.count; i++) {
                    if (t <= keys.data[i + 1].time) {
                        double seg = keys.data[i + 1].time - keys.data[i].time;
                        float frac = (seg > 1e-9) ? (float)((t - keys.data[i].time) / seg) : 0.0f;
                        auto& a = keys.data[i].value;
                        auto& b = keys.data[i + 1].value;
                        return glm::vec3(
                            glm::mix((float)a.x, (float)b.x, frac),
                            glm::mix((float)a.y, (float)b.y, frac),
                            glm::mix((float)a.z, (float)b.z, frac));
                    }
                }
                auto& last = keys.data[keys.count - 1].value;
                return glm::vec3((float)last.x, (float)last.y, (float)last.z);
            };

            auto sampleQuat = [](const ufbx_baked_quat_list& keys, double t) -> glm::quat {
                if (keys.count == 0) return glm::quat(1, 0, 0, 0);
                if (keys.count == 1 || t <= keys.data[0].time) {
                    auto& q = keys.data[0].value;
                    return glm::quat((float)q.w, (float)q.x, (float)q.y, (float)q.z);
                }
                for (size_t i = 0; i + 1 < keys.count; i++) {
                    if (t <= keys.data[i + 1].time) {
                        double seg = keys.data[i + 1].time - keys.data[i].time;
                        float frac = (seg > 1e-9) ? (float)((t - keys.data[i].time) / seg) : 0.0f;
                        auto& a = keys.data[i].value;
                        auto& b = keys.data[i + 1].value;
                        glm::quat qa((float)a.w, (float)a.x, (float)a.y, (float)a.z);
                        glm::quat qb((float)b.w, (float)b.x, (float)b.y, (float)b.z);
                        return glm::slerp(qa, qb, frac);
                    }
                }
                auto& q = keys.data[keys.count - 1].value;
                return glm::quat((float)q.w, (float)q.x, (float)q.y, (float)q.z);
            };

            for (double t : times) {
                BoneKeyframe kf;
                kf.Time = (float)(t - keyMin);
                kf.Position = sampleVec3(bakedNode->translation_keys, t);
                kf.Rotation = sampleQuat(bakedNode->rotation_keys, t);
                kf.Scale = sampleVec3(bakedNode->scale_keys, t);
                if (bakedNode->scale_keys.count == 0)
                    kf.Scale = glm::vec3(1.0f);
                track.Keyframes.push_back(kf);
            }

            if (!track.Keyframes.empty())
                clip.Tracks.push_back(std::move(track));
        }

        ufbx_free_baked_anim(baked);

        if (!clip.Tracks.empty()) {
            VE_ENGINE_INFO("FBXImporter::ImportAnimations: clip '{0}' — {1:.2f}s, {2} tracks",
                clip.Name, clip.Duration, clip.Tracks.size());
            result.push_back(std::move(clip));
        }
    }

    ufbx_free_scene(scene);
    return result;
}

} // namespace VE
