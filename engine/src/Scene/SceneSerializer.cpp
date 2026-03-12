#include "VibeEngine/Scene/SceneSerializer.h"
#include "VibeEngine/Scene/Entity.h"
#include "VibeEngine/Scene/Components.h"
#include "VibeEngine/Scene/MeshLibrary.h"
#include "VibeEngine/Asset/MeshImporter.h"
#include "VibeEngine/Renderer/Material.h"
#include "VibeEngine/Core/Log.h"

#include <yaml-cpp/yaml.h>
#include <fstream>

namespace VE {

SceneSerializer::SceneSerializer(const std::shared_ptr<Scene>& scene)
    : m_Scene(scene) {}

// ── Helpers ────────────────────────────────────────────────────────

static void SerializeEntity(YAML::Emitter& out, Entity entity, entt::registry& registry) {
    out << YAML::BeginMap;

    // UUID
    auto& id = entity.GetComponent<IDComponent>();
    out << YAML::Key << "Entity" << YAML::Value << static_cast<uint64_t>(id.ID);

    // Parent UUID
    if (entity.HasComponent<RelationshipComponent>()) {
        auto& rel = entity.GetComponent<RelationshipComponent>();
        if (rel.Parent != entt::null && registry.valid(rel.Parent)) {
            uint64_t parentUUID = static_cast<uint64_t>(registry.get<IDComponent>(rel.Parent).ID);
            out << YAML::Key << "Parent" << YAML::Value << parentUUID;
        }
    }

    // TagComponent
    if (entity.HasComponent<TagComponent>()) {
        out << YAML::Key << "TagComponent" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Tag" << YAML::Value << entity.GetComponent<TagComponent>().Tag;
        out << YAML::EndMap;
    }

    // TransformComponent
    if (entity.HasComponent<TransformComponent>()) {
        auto& tc = entity.GetComponent<TransformComponent>();
        out << YAML::Key << "TransformComponent" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Position" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << tc.Position[0] << tc.Position[1] << tc.Position[2] << YAML::EndSeq;
        out << YAML::Key << "Rotation" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << tc.Rotation[0] << tc.Rotation[1] << tc.Rotation[2] << YAML::EndSeq;
        out << YAML::Key << "Scale" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << tc.Scale[0] << tc.Scale[1] << tc.Scale[2] << YAML::EndSeq;
        out << YAML::EndMap;
    }

    // DirectionalLightComponent
    if (entity.HasComponent<DirectionalLightComponent>()) {
        auto& dl = entity.GetComponent<DirectionalLightComponent>();
        out << YAML::Key << "DirectionalLightComponent" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Direction" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << dl.Direction[0] << dl.Direction[1] << dl.Direction[2] << YAML::EndSeq;
        out << YAML::Key << "Color" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << dl.Color[0] << dl.Color[1] << dl.Color[2] << YAML::EndSeq;
        out << YAML::Key << "Intensity" << YAML::Value << dl.Intensity;
        out << YAML::EndMap;
    }

    // RigidbodyComponent
    if (entity.HasComponent<RigidbodyComponent>()) {
        auto& rb = entity.GetComponent<RigidbodyComponent>();
        out << YAML::Key << "RigidbodyComponent" << YAML::Value << YAML::BeginMap;
        const char* bodyTypeStr = rb.Type == BodyType::Static ? "Static" :
                                  rb.Type == BodyType::Kinematic ? "Kinematic" : "Dynamic";
        out << YAML::Key << "BodyType"       << YAML::Value << bodyTypeStr;
        out << YAML::Key << "Mass"           << YAML::Value << rb.Mass;
        out << YAML::Key << "LinearDamping"  << YAML::Value << rb.LinearDamping;
        out << YAML::Key << "AngularDamping" << YAML::Value << rb.AngularDamping;
        out << YAML::Key << "Restitution"    << YAML::Value << rb.Restitution;
        out << YAML::Key << "Friction"       << YAML::Value << rb.Friction;
        out << YAML::Key << "UseGravity"     << YAML::Value << rb.UseGravity;
        out << YAML::EndMap;
    }

    // ColliderComponent
    if (entity.HasComponent<ColliderComponent>()) {
        auto& col = entity.GetComponent<ColliderComponent>();
        out << YAML::Key << "ColliderComponent" << YAML::Value << YAML::BeginMap;
        const char* shapeStr = col.Shape == ColliderShape::Box ? "Box" :
                               col.Shape == ColliderShape::Sphere ? "Sphere" : "Capsule";
        out << YAML::Key << "Shape"  << YAML::Value << shapeStr;
        out << YAML::Key << "Size"   << YAML::Value << YAML::Flow
            << YAML::BeginSeq << col.Size[0] << col.Size[1] << col.Size[2] << YAML::EndSeq;
        out << YAML::Key << "Offset" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << col.Offset[0] << col.Offset[1] << col.Offset[2] << YAML::EndSeq;
        out << YAML::EndMap;
    }

    // ScriptComponent
    if (entity.HasComponent<ScriptComponent>()) {
        auto& sc = entity.GetComponent<ScriptComponent>();
        out << YAML::Key << "ScriptComponent" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "ClassName" << YAML::Value << sc.ClassName;
        out << YAML::EndMap;
    }

    // MeshRendererComponent
    if (entity.HasComponent<MeshRendererComponent>()) {
        auto& mr = entity.GetComponent<MeshRendererComponent>();
        out << YAML::Key << "MeshRendererComponent" << YAML::Value << YAML::BeginMap;

        // Find mesh index in MeshLibrary
        int meshIndex = -1;
        for (int i = 0; i < MeshLibrary::GetMeshCount(); i++) {
            if (MeshLibrary::GetMeshByIndex(i) == mr.Mesh) {
                meshIndex = i;
                break;
            }
        }
        out << YAML::Key << "MeshType" << YAML::Value << meshIndex;
        if (meshIndex == -1 && !mr.MeshSourcePath.empty())
            out << YAML::Key << "MeshSource" << YAML::Value << mr.MeshSourcePath;

        out << YAML::Key << "Color" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << mr.Color[0] << mr.Color[1] << mr.Color[2] << mr.Color[3] << YAML::EndSeq;

        // Material reference
        if (mr.Mat) {
            if (!mr.MaterialPath.empty())
                out << YAML::Key << "MaterialPath" << YAML::Value << mr.MaterialPath;
            else
                out << YAML::Key << "MaterialName" << YAML::Value << mr.Mat->GetName();
        }

        out << YAML::EndMap;
    }

    out << YAML::EndMap;
}

// ── Serialize ──────────────────────────────────────────────────────

static std::string SerializeSceneToYAML(const std::shared_ptr<Scene>& scene) {
    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "Scene" << YAML::Value << "Untitled";

    // Pipeline settings
    auto& ps = scene->GetPipelineSettings();
    out << YAML::Key << "PipelineSettings" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "SkyEnabled" << YAML::Value << ps.SkyEnabled;
    out << YAML::Key << "SkyTopColor" << YAML::Value << YAML::Flow
        << YAML::BeginSeq << ps.SkyTopColor[0] << ps.SkyTopColor[1] << ps.SkyTopColor[2] << YAML::EndSeq;
    out << YAML::Key << "SkyBottomColor" << YAML::Value << YAML::Flow
        << YAML::BeginSeq << ps.SkyBottomColor[0] << ps.SkyBottomColor[1] << ps.SkyBottomColor[2] << YAML::EndSeq;
    if (!ps.SkyTexturePath.empty())
        out << YAML::Key << "SkyTexturePath" << YAML::Value << ps.SkyTexturePath;
    out << YAML::Key << "BloomEnabled" << YAML::Value << ps.BloomEnabled;
    out << YAML::Key << "BloomThreshold" << YAML::Value << ps.BloomThreshold;
    out << YAML::Key << "BloomIntensity" << YAML::Value << ps.BloomIntensity;
    out << YAML::Key << "BloomIterations" << YAML::Value << ps.BloomIterations;
    out << YAML::Key << "VignetteEnabled" << YAML::Value << ps.VignetteEnabled;
    out << YAML::Key << "VignetteIntensity" << YAML::Value << ps.VignetteIntensity;
    out << YAML::Key << "VignetteSmoothness" << YAML::Value << ps.VignetteSmoothness;
    out << YAML::Key << "ColorAdjustEnabled" << YAML::Value << ps.ColorAdjustEnabled;
    out << YAML::Key << "ColorExposure" << YAML::Value << ps.ColorExposure;
    out << YAML::Key << "ColorContrast" << YAML::Value << ps.ColorContrast;
    out << YAML::Key << "ColorSaturation" << YAML::Value << ps.ColorSaturation;
    out << YAML::Key << "ColorFilter" << YAML::Value << YAML::Flow
        << YAML::BeginSeq << ps.ColorFilter[0] << ps.ColorFilter[1] << ps.ColorFilter[2] << YAML::EndSeq;
    out << YAML::Key << "ColorGamma" << YAML::Value << ps.ColorGamma;
    out << YAML::Key << "SMHEnabled" << YAML::Value << ps.SMHEnabled;
    out << YAML::Key << "SMH_Shadows" << YAML::Value << YAML::Flow
        << YAML::BeginSeq << ps.SMH_Shadows[0] << ps.SMH_Shadows[1] << ps.SMH_Shadows[2] << YAML::EndSeq;
    out << YAML::Key << "SMH_Midtones" << YAML::Value << YAML::Flow
        << YAML::BeginSeq << ps.SMH_Midtones[0] << ps.SMH_Midtones[1] << ps.SMH_Midtones[2] << YAML::EndSeq;
    out << YAML::Key << "SMH_Highlights" << YAML::Value << YAML::Flow
        << YAML::BeginSeq << ps.SMH_Highlights[0] << ps.SMH_Highlights[1] << ps.SMH_Highlights[2] << YAML::EndSeq;
    out << YAML::Key << "SMH_ShadowStart" << YAML::Value << ps.SMH_ShadowStart;
    out << YAML::Key << "SMH_ShadowEnd" << YAML::Value << ps.SMH_ShadowEnd;
    out << YAML::Key << "SMH_HighlightStart" << YAML::Value << ps.SMH_HighlightStart;
    out << YAML::Key << "SMH_HighlightEnd" << YAML::Value << ps.SMH_HighlightEnd;
    out << YAML::Key << "CurvesEnabled" << YAML::Value << ps.CurvesEnabled;
    auto serializeCurve = [&](const std::string& key, const std::vector<std::pair<float,float>>& pts) {
        out << YAML::Key << key << YAML::Value << YAML::BeginSeq;
        for (auto& p : pts)
            out << YAML::Flow << YAML::BeginSeq << p.first << p.second << YAML::EndSeq;
        out << YAML::EndSeq;
    };
    serializeCurve("CurvesMaster", ps.CurvesMaster);
    serializeCurve("CurvesRed", ps.CurvesRed);
    serializeCurve("CurvesGreen", ps.CurvesGreen);
    serializeCurve("CurvesBlue", ps.CurvesBlue);
    out << YAML::Key << "TonemapEnabled" << YAML::Value << ps.TonemapEnabled;
    out << YAML::Key << "TonemapMode" << YAML::Value << ps.TonemapMode;
    out << YAML::EndMap;

    out << YAML::Key << "Entities" << YAML::Value << YAML::BeginSeq;

    auto view = scene->GetAllEntitiesWith<IDComponent>();
    for (auto entityID : view) {
        Entity entity(entityID, &*scene);
        SerializeEntity(out, entity, scene->GetRegistry());
    }

    out << YAML::EndSeq;
    out << YAML::EndMap;
    return std::string(out.c_str());
}

static bool DeserializeSceneFromYAML(const YAML::Node& data, const std::shared_ptr<Scene>& scene) {
    if (!data["Scene"])
        return false;

    // Pipeline settings
    if (auto psNode = data["PipelineSettings"]) {
        auto& ps = scene->GetPipelineSettings();
        if (psNode["SkyEnabled"]) ps.SkyEnabled = psNode["SkyEnabled"].as<bool>();
        if (auto top = psNode["SkyTopColor"])
            ps.SkyTopColor = { top[0].as<float>(), top[1].as<float>(), top[2].as<float>() };
        if (auto bot = psNode["SkyBottomColor"])
            ps.SkyBottomColor = { bot[0].as<float>(), bot[1].as<float>(), bot[2].as<float>() };
        if (auto tex = psNode["SkyTexturePath"]) {
            ps.SkyTexturePath = tex.as<std::string>();
            ps.SkyTexture = Texture2D::Create(ps.SkyTexturePath);
        }
        if (psNode["BloomEnabled"]) ps.BloomEnabled = psNode["BloomEnabled"].as<bool>();
        if (psNode["BloomThreshold"]) ps.BloomThreshold = psNode["BloomThreshold"].as<float>();
        if (psNode["BloomIntensity"]) ps.BloomIntensity = psNode["BloomIntensity"].as<float>();
        if (psNode["BloomIterations"]) ps.BloomIterations = psNode["BloomIterations"].as<int>();
        if (psNode["VignetteEnabled"]) ps.VignetteEnabled = psNode["VignetteEnabled"].as<bool>();
        if (psNode["VignetteIntensity"]) ps.VignetteIntensity = psNode["VignetteIntensity"].as<float>();
        if (psNode["VignetteSmoothness"]) ps.VignetteSmoothness = psNode["VignetteSmoothness"].as<float>();
        if (psNode["ColorAdjustEnabled"]) ps.ColorAdjustEnabled = psNode["ColorAdjustEnabled"].as<bool>();
        if (psNode["ColorExposure"]) ps.ColorExposure = psNode["ColorExposure"].as<float>();
        if (psNode["ColorContrast"]) ps.ColorContrast = psNode["ColorContrast"].as<float>();
        if (psNode["ColorSaturation"]) ps.ColorSaturation = psNode["ColorSaturation"].as<float>();
        if (auto cf = psNode["ColorFilter"])
            ps.ColorFilter = { cf[0].as<float>(), cf[1].as<float>(), cf[2].as<float>() };
        if (psNode["ColorGamma"]) ps.ColorGamma = psNode["ColorGamma"].as<float>();
        if (psNode["SMHEnabled"]) ps.SMHEnabled = psNode["SMHEnabled"].as<bool>();
        if (auto v = psNode["SMH_Shadows"])
            ps.SMH_Shadows = { v[0].as<float>(), v[1].as<float>(), v[2].as<float>() };
        if (auto v = psNode["SMH_Midtones"])
            ps.SMH_Midtones = { v[0].as<float>(), v[1].as<float>(), v[2].as<float>() };
        if (auto v = psNode["SMH_Highlights"])
            ps.SMH_Highlights = { v[0].as<float>(), v[1].as<float>(), v[2].as<float>() };
        if (psNode["SMH_ShadowStart"]) ps.SMH_ShadowStart = psNode["SMH_ShadowStart"].as<float>();
        if (psNode["SMH_ShadowEnd"]) ps.SMH_ShadowEnd = psNode["SMH_ShadowEnd"].as<float>();
        if (psNode["SMH_HighlightStart"]) ps.SMH_HighlightStart = psNode["SMH_HighlightStart"].as<float>();
        if (psNode["SMH_HighlightEnd"]) ps.SMH_HighlightEnd = psNode["SMH_HighlightEnd"].as<float>();
        if (psNode["CurvesEnabled"]) ps.CurvesEnabled = psNode["CurvesEnabled"].as<bool>();
        auto deserializeCurve = [&](const std::string& key, std::vector<std::pair<float,float>>& pts) {
            if (auto node = psNode[key]) {
                pts.clear();
                for (auto pt : node)
                    pts.push_back({ pt[0].as<float>(), pt[1].as<float>() });
            }
        };
        deserializeCurve("CurvesMaster", ps.CurvesMaster);
        deserializeCurve("CurvesRed", ps.CurvesRed);
        deserializeCurve("CurvesGreen", ps.CurvesGreen);
        deserializeCurve("CurvesBlue", ps.CurvesBlue);
        if (psNode["TonemapEnabled"]) ps.TonemapEnabled = psNode["TonemapEnabled"].as<bool>();
        if (psNode["TonemapMode"]) ps.TonemapMode = psNode["TonemapMode"].as<int>();
    }

    auto entities = data["Entities"];
    if (!entities)
        return true;

    for (auto entityNode : entities) {
        uint64_t uuid = entityNode["Entity"].as<uint64_t>();

        std::string name = "GameObject";
        if (auto tagNode = entityNode["TagComponent"])
            name = tagNode["Tag"].as<std::string>();

        Entity entity = scene->CreateEntityWithUUID(UUID(uuid), name);

        if (auto tcNode = entityNode["TransformComponent"]) {
            auto& tc = entity.GetComponent<TransformComponent>();
            auto pos = tcNode["Position"];
            tc.Position = { pos[0].as<float>(), pos[1].as<float>(), pos[2].as<float>() };
            auto rot = tcNode["Rotation"];
            tc.Rotation = { rot[0].as<float>(), rot[1].as<float>(), rot[2].as<float>() };
            auto scl = tcNode["Scale"];
            tc.Scale = { scl[0].as<float>(), scl[1].as<float>(), scl[2].as<float>() };
        }

        if (auto dlNode = entityNode["DirectionalLightComponent"]) {
            auto& dl = entity.AddComponent<DirectionalLightComponent>();
            auto dir = dlNode["Direction"];
            dl.Direction = { dir[0].as<float>(), dir[1].as<float>(), dir[2].as<float>() };
            auto col = dlNode["Color"];
            dl.Color = { col[0].as<float>(), col[1].as<float>(), col[2].as<float>() };
            dl.Intensity = dlNode["Intensity"].as<float>();
        }

        if (auto rbNode = entityNode["RigidbodyComponent"]) {
            auto& rb = entity.AddComponent<RigidbodyComponent>();
            std::string bt = rbNode["BodyType"].as<std::string>();
            if (bt == "Static") rb.Type = BodyType::Static;
            else if (bt == "Kinematic") rb.Type = BodyType::Kinematic;
            else rb.Type = BodyType::Dynamic;
            rb.Mass           = rbNode["Mass"].as<float>();
            rb.LinearDamping  = rbNode["LinearDamping"].as<float>();
            rb.AngularDamping = rbNode["AngularDamping"].as<float>();
            rb.Restitution    = rbNode["Restitution"].as<float>();
            rb.Friction       = rbNode["Friction"].as<float>();
            rb.UseGravity     = rbNode["UseGravity"].as<bool>();
        }

        if (auto colNode = entityNode["ColliderComponent"]) {
            auto& col = entity.AddComponent<ColliderComponent>();
            std::string sh = colNode["Shape"].as<std::string>();
            if (sh == "Box") col.Shape = ColliderShape::Box;
            else if (sh == "Sphere") col.Shape = ColliderShape::Sphere;
            else col.Shape = ColliderShape::Capsule;
            auto sz = colNode["Size"];
            col.Size = { sz[0].as<float>(), sz[1].as<float>(), sz[2].as<float>() };
            auto off = colNode["Offset"];
            col.Offset = { off[0].as<float>(), off[1].as<float>(), off[2].as<float>() };
        }

        if (auto scNode = entityNode["ScriptComponent"]) {
            auto& sc = entity.AddComponent<ScriptComponent>();
            sc.ClassName = scNode["ClassName"].as<std::string>();
        }

        if (auto mrNode = entityNode["MeshRendererComponent"]) {
            auto& mr = entity.AddComponent<MeshRendererComponent>();
            int meshIndex = mrNode["MeshType"].as<int>();
            if (meshIndex >= 0 && meshIndex < MeshLibrary::GetMeshCount()) {
                mr.Mesh = MeshLibrary::GetMeshByIndex(meshIndex);
                mr.Mat = MeshLibrary::IsLitMesh(meshIndex)
                    ? MaterialLibrary::Get("Lit")
                    : MaterialLibrary::Get("Default");
            } else if (meshIndex == -1 && mrNode["MeshSource"]) {
                mr.MeshSourcePath = mrNode["MeshSource"].as<std::string>();
                auto meshAsset = MeshImporter::GetOrLoad(mr.MeshSourcePath);
                if (meshAsset && meshAsset->VAO) {
                    mr.Mesh = meshAsset->VAO;
                    mr.Mat = MaterialLibrary::Get("Lit");
                }
            }
            if (auto colorNode = mrNode["Color"]) {
                mr.Color = {
                    colorNode[0].as<float>(), colorNode[1].as<float>(),
                    colorNode[2].as<float>(), colorNode[3].as<float>()
                };
            }
            // Material reference
            if (auto matPathNode = mrNode["MaterialPath"]) {
                mr.MaterialPath = matPathNode.as<std::string>();
                auto mat = Material::Load(mr.MaterialPath);
                if (mat) {
                    mr.Mat = mat;
                    MaterialLibrary::Register(mat);
                }
            } else if (auto matNameNode = mrNode["MaterialName"]) {
                auto mat = MaterialLibrary::Get(matNameNode.as<std::string>());
                if (mat) mr.Mat = mat;
            }
            // Backward compat: old TexturePath field → set as material texture
            if (auto texNode = mrNode["TexturePath"]) {
                std::string texPath = texNode.as<std::string>();
                if (!texPath.empty() && mr.Mat)
                    mr.Mat->SetTexture("u_Texture", texPath);
            }
        }
    }

    // Second pass: resolve parent-child relationships by UUID
    {
        // Build UUID -> entity handle map
        std::unordered_map<uint64_t, entt::entity> uuidMap;
        auto idView = scene->GetAllEntitiesWith<IDComponent>();
        for (auto e : idView)
            uuidMap[static_cast<uint64_t>(idView.get<IDComponent>(e).ID)] = e;

        // Re-iterate YAML to find Parent fields
        for (auto entityNode : entities) {
            if (!entityNode["Parent"]) continue;
            uint64_t childUUID  = entityNode["Entity"].as<uint64_t>();
            uint64_t parentUUID = entityNode["Parent"].as<uint64_t>();
            auto childIt  = uuidMap.find(childUUID);
            auto parentIt = uuidMap.find(parentUUID);
            if (childIt != uuidMap.end() && parentIt != uuidMap.end())
                scene->SetParent(childIt->second, parentIt->second);
        }
    }

    return true;
}

void SceneSerializer::Serialize(const std::string& filepath) {
    std::string yaml = SerializeSceneToYAML(m_Scene);
    std::ofstream fout(filepath);
    fout << yaml;
    fout.close();
    VE_ENGINE_INFO("Scene saved to: {0}", filepath);
}

// ── Deserialize ────────────────────────────────────────────────────

bool SceneSerializer::Deserialize(const std::string& filepath) {
    YAML::Node data;
    try {
        data = YAML::LoadFile(filepath);
    } catch (const YAML::Exception& e) {
        VE_ENGINE_ERROR("Failed to load scene '{0}': {1}", filepath, e.what());
        return false;
    }

    if (!DeserializeSceneFromYAML(data, m_Scene)) {
        VE_ENGINE_ERROR("Invalid scene file: {0}", filepath);
        return false;
    }

    VE_ENGINE_INFO("Scene loaded from: {0}", filepath);
    return true;
}

// ── In-memory snapshot ─────────────────────────────────────────────

std::string SceneSerializer::SerializeToString() {
    return SerializeSceneToYAML(m_Scene);
}

bool SceneSerializer::DeserializeFromString(const std::string& yamlData) {
    YAML::Node data;
    try {
        data = YAML::Load(yamlData);
    } catch (const YAML::Exception& e) {
        VE_ENGINE_ERROR("Failed to parse scene snapshot: {0}", e.what());
        return false;
    }
    return DeserializeSceneFromYAML(data, m_Scene);
}

} // namespace VE
