#include "VibeEngine/Scene/SceneSerializer.h"
#include "VibeEngine/Scene/Entity.h"
#include "VibeEngine/Scene/Components.h"
#include "VibeEngine/Scene/MeshLibrary.h"
#include "VibeEngine/Core/Log.h"

#include <yaml-cpp/yaml.h>
#include <fstream>

namespace VE {

SceneSerializer::SceneSerializer(const std::shared_ptr<Scene>& scene)
    : m_Scene(scene) {}

// ── Helpers ────────────────────────────────────────────────────────

static void SerializeEntity(YAML::Emitter& out, Entity entity) {
    out << YAML::BeginMap;

    // UUID
    auto& id = entity.GetComponent<IDComponent>();
    out << YAML::Key << "Entity" << YAML::Value << static_cast<uint64_t>(id.ID);

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

        out << YAML::Key << "Color" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << mr.Color[0] << mr.Color[1] << mr.Color[2] << mr.Color[3] << YAML::EndSeq;

        if (!mr.TexturePath.empty())
            out << YAML::Key << "TexturePath" << YAML::Value << mr.TexturePath;

        out << YAML::EndMap;
    }

    out << YAML::EndMap;
}

// ── Serialize ──────────────────────────────────────────────────────

void SceneSerializer::Serialize(const std::string& filepath) {
    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "Scene" << YAML::Value << "Untitled";
    out << YAML::Key << "Entities" << YAML::Value << YAML::BeginSeq;

    auto view = m_Scene->GetAllEntitiesWith<IDComponent>();
    for (auto entityID : view) {
        Entity entity(entityID, &*m_Scene);
        SerializeEntity(out, entity);
    }

    out << YAML::EndSeq;
    out << YAML::EndMap;

    std::ofstream fout(filepath);
    fout << out.c_str();
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

    if (!data["Scene"]) {
        VE_ENGINE_ERROR("Invalid scene file: {0}", filepath);
        return false;
    }

    auto entities = data["Entities"];
    if (!entities)
        return true; // Empty scene is valid

    for (auto entityNode : entities) {
        uint64_t uuid = entityNode["Entity"].as<uint64_t>();

        std::string name = "GameObject";
        if (auto tagNode = entityNode["TagComponent"]) {
            name = tagNode["Tag"].as<std::string>();
        }

        Entity entity = m_Scene->CreateEntityWithUUID(UUID(uuid), name);

        // TransformComponent (already added by CreateEntityWithUUID)
        if (auto tcNode = entityNode["TransformComponent"]) {
            auto& tc = entity.GetComponent<TransformComponent>();
            auto pos = tcNode["Position"];
            tc.Position = { pos[0].as<float>(), pos[1].as<float>(), pos[2].as<float>() };
            auto rot = tcNode["Rotation"];
            tc.Rotation = { rot[0].as<float>(), rot[1].as<float>(), rot[2].as<float>() };
            auto scl = tcNode["Scale"];
            tc.Scale = { scl[0].as<float>(), scl[1].as<float>(), scl[2].as<float>() };
        }

        // MeshRendererComponent
        if (auto mrNode = entityNode["MeshRendererComponent"]) {
            auto& mr = entity.AddComponent<MeshRendererComponent>();
            int meshIndex = mrNode["MeshType"].as<int>();
            if (meshIndex >= 0 && meshIndex < MeshLibrary::GetMeshCount()) {
                mr.Mesh = MeshLibrary::GetMeshByIndex(meshIndex);
                mr.Material = MeshLibrary::IsLitMesh(meshIndex)
                    ? MeshLibrary::GetLitShader()
                    : MeshLibrary::GetDefaultShader();
            }
            if (auto colorNode = mrNode["Color"]) {
                mr.Color = {
                    colorNode[0].as<float>(), colorNode[1].as<float>(),
                    colorNode[2].as<float>(), colorNode[3].as<float>()
                };
            }
            if (auto texNode = mrNode["TexturePath"]) {
                mr.TexturePath = texNode.as<std::string>();
                mr.Texture = Texture2D::Create(mr.TexturePath);
            }
        }
    }

    VE_ENGINE_INFO("Scene loaded from: {0}", filepath);
    return true;
}

} // namespace VE
