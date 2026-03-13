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
        auto& tc = entity.GetComponent<TagComponent>();
        out << YAML::Key << "TagComponent" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Tag" << YAML::Value << tc.Tag;
        out << YAML::Key << "GameObjectTag" << YAML::Value << tc.GameObjectTag;
        out << YAML::Key << "Layer" << YAML::Value << tc.Layer;
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

    // PointLightComponent
    if (entity.HasComponent<PointLightComponent>()) {
        auto& pl = entity.GetComponent<PointLightComponent>();
        out << YAML::Key << "PointLightComponent" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Color" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << pl.Color[0] << pl.Color[1] << pl.Color[2] << YAML::EndSeq;
        out << YAML::Key << "Intensity" << YAML::Value << pl.Intensity;
        out << YAML::Key << "Range" << YAML::Value << pl.Range;
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

    // Collider components (one per type)
    if (entity.HasComponent<BoxColliderComponent>()) {
        auto& col = entity.GetComponent<BoxColliderComponent>();
        out << YAML::Key << "BoxColliderComponent" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Size"   << YAML::Value << YAML::Flow
            << YAML::BeginSeq << col.Size[0] << col.Size[1] << col.Size[2] << YAML::EndSeq;
        out << YAML::Key << "Offset" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << col.Offset[0] << col.Offset[1] << col.Offset[2] << YAML::EndSeq;
        out << YAML::EndMap;
    }
    if (entity.HasComponent<SphereColliderComponent>()) {
        auto& col = entity.GetComponent<SphereColliderComponent>();
        out << YAML::Key << "SphereColliderComponent" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Radius" << YAML::Value << col.Radius;
        out << YAML::Key << "Offset" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << col.Offset[0] << col.Offset[1] << col.Offset[2] << YAML::EndSeq;
        out << YAML::EndMap;
    }
    if (entity.HasComponent<CapsuleColliderComponent>()) {
        auto& col = entity.GetComponent<CapsuleColliderComponent>();
        out << YAML::Key << "CapsuleColliderComponent" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Radius" << YAML::Value << col.Radius;
        out << YAML::Key << "Height" << YAML::Value << col.Height;
        out << YAML::Key << "Offset" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << col.Offset[0] << col.Offset[1] << col.Offset[2] << YAML::EndSeq;
        out << YAML::EndMap;
    }
    if (entity.HasComponent<MeshColliderComponent>()) {
        auto& col = entity.GetComponent<MeshColliderComponent>();
        out << YAML::Key << "MeshColliderComponent" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Convex" << YAML::Value << col.Convex;
        out << YAML::Key << "Offset" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << col.Offset[0] << col.Offset[1] << col.Offset[2] << YAML::EndSeq;
        out << YAML::EndMap;
    }

    // ScriptComponent
    if (entity.HasComponent<ScriptComponent>()) {
        auto& sc = entity.GetComponent<ScriptComponent>();
        out << YAML::Key << "ScriptComponent" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "ClassName" << YAML::Value << sc.ClassName;
        if (!sc.Properties.empty()) {
            out << YAML::Key << "Properties" << YAML::Value << YAML::BeginMap;
            for (auto& [name, val] : sc.Properties) {
                if (std::holds_alternative<float>(val))
                    out << YAML::Key << name << YAML::Value << std::get<float>(val);
                else if (std::holds_alternative<int>(val))
                    out << YAML::Key << name << YAML::Value << std::get<int>(val);
                else if (std::holds_alternative<bool>(val))
                    out << YAML::Key << name << YAML::Value << std::get<bool>(val);
            }
            out << YAML::EndMap;
        }
        out << YAML::EndMap;
    }

    // CameraComponent
    if (entity.HasComponent<CameraComponent>()) {
        auto& cam = entity.GetComponent<CameraComponent>();
        out << YAML::Key << "CameraComponent" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "ProjectionType" << YAML::Value
            << (cam.ProjectionType == CameraProjection::Perspective ? "Perspective" : "Orthographic");
        out << YAML::Key << "FOV"      << YAML::Value << cam.FOV;
        out << YAML::Key << "Size"     << YAML::Value << cam.Size;
        out << YAML::Key << "NearClip" << YAML::Value << cam.NearClip;
        out << YAML::Key << "FarClip"  << YAML::Value << cam.FarClip;
        out << YAML::Key << "Priority" << YAML::Value << cam.Priority;
        out << YAML::EndMap;
    }

    // AudioSourceComponent
    if (entity.HasComponent<AudioSourceComponent>()) {
        auto& as = entity.GetComponent<AudioSourceComponent>();
        out << YAML::Key << "AudioSourceComponent" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "ClipPath" << YAML::Value << as.ClipPath;
        out << YAML::Key << "Volume" << YAML::Value << as.Volume;
        out << YAML::Key << "Pitch" << YAML::Value << as.Pitch;
        out << YAML::Key << "Loop" << YAML::Value << as.Loop;
        out << YAML::Key << "Spatial" << YAML::Value << as.Spatial;
        out << YAML::Key << "PlayOnAwake" << YAML::Value << as.PlayOnAwake;
        out << YAML::Key << "MinDistance" << YAML::Value << as.MinDistance;
        out << YAML::Key << "MaxDistance" << YAML::Value << as.MaxDistance;
        out << YAML::EndMap;
    }

    // AudioListenerComponent
    if (entity.HasComponent<AudioListenerComponent>()) {
        out << YAML::Key << "AudioListenerComponent" << YAML::Value << YAML::BeginMap;
        out << YAML::EndMap;
    }

    // SpriteRendererComponent
    if (entity.HasComponent<SpriteRendererComponent>()) {
        auto& sr = entity.GetComponent<SpriteRendererComponent>();
        out << YAML::Key << "SpriteRendererComponent" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Color" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << sr.Color[0] << sr.Color[1] << sr.Color[2] << sr.Color[3] << YAML::EndSeq;
        if (!sr.TexturePath.empty())
            out << YAML::Key << "TexturePath" << YAML::Value << sr.TexturePath;
        out << YAML::Key << "UVRect" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << sr.UVRect[0] << sr.UVRect[1] << sr.UVRect[2] << sr.UVRect[3] << YAML::EndSeq;
        out << YAML::Key << "SortingOrder" << YAML::Value << sr.SortingOrder;
        out << YAML::EndMap;
    }

    // SpriteAnimatorComponent
    if (entity.HasComponent<SpriteAnimatorComponent>()) {
        auto& sa = entity.GetComponent<SpriteAnimatorComponent>();
        out << YAML::Key << "SpriteAnimatorComponent" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Columns" << YAML::Value << sa.Columns;
        out << YAML::Key << "Rows" << YAML::Value << sa.Rows;
        out << YAML::Key << "StartFrame" << YAML::Value << sa.StartFrame;
        out << YAML::Key << "EndFrame" << YAML::Value << sa.EndFrame;
        out << YAML::Key << "FrameRate" << YAML::Value << sa.FrameRate;
        out << YAML::Key << "Loop" << YAML::Value << sa.Loop;
        out << YAML::Key << "PlayOnStart" << YAML::Value << sa.PlayOnStart;
        out << YAML::EndMap;
    }

    // ParticleSystemComponent
    if (entity.HasComponent<ParticleSystemComponent>()) {
        auto& ps = entity.GetComponent<ParticleSystemComponent>();
        out << YAML::Key << "ParticleSystemComponent" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "EmissionRate" << YAML::Value << ps.EmissionRate;
        out << YAML::Key << "ParticleLifetime" << YAML::Value << ps.ParticleLifetime;
        out << YAML::Key << "LifetimeVariance" << YAML::Value << ps.LifetimeVariance;
        out << YAML::Key << "MaxParticles" << YAML::Value << ps.MaxParticles;
        out << YAML::Key << "VelocityMin" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << ps.VelocityMin[0] << ps.VelocityMin[1] << ps.VelocityMin[2] << YAML::EndSeq;
        out << YAML::Key << "VelocityMax" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << ps.VelocityMax[0] << ps.VelocityMax[1] << ps.VelocityMax[2] << YAML::EndSeq;
        out << YAML::Key << "Gravity" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << ps.Gravity[0] << ps.Gravity[1] << ps.Gravity[2] << YAML::EndSeq;
        out << YAML::Key << "StartColor" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << ps.StartColor[0] << ps.StartColor[1] << ps.StartColor[2] << ps.StartColor[3] << YAML::EndSeq;
        out << YAML::Key << "EndColor" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << ps.EndColor[0] << ps.EndColor[1] << ps.EndColor[2] << ps.EndColor[3] << YAML::EndSeq;
        out << YAML::Key << "StartSize" << YAML::Value << ps.StartSize;
        out << YAML::Key << "EndSize" << YAML::Value << ps.EndSize;
        if (!ps.TexturePath.empty())
            out << YAML::Key << "TexturePath" << YAML::Value << ps.TexturePath;
        out << YAML::Key << "PlayOnStart" << YAML::Value << ps.PlayOnStart;
        out << YAML::EndMap;
    }

    // AnimatorComponent
    if (entity.HasComponent<AnimatorComponent>()) {
        auto& ac = entity.GetComponent<AnimatorComponent>();
        out << YAML::Key << "AnimatorComponent" << YAML::Value << YAML::BeginMap;
        if (!ac.AnimationSourcePath.empty())
            out << YAML::Key << "AnimationSource" << YAML::Value << ac.AnimationSourcePath;
        out << YAML::Key << "ClipIndex" << YAML::Value << ac.ClipIndex;
        out << YAML::Key << "PlayOnStart" << YAML::Value << ac.PlayOnStart;
        out << YAML::Key << "Loop" << YAML::Value << ac.Loop;
        out << YAML::Key << "Speed" << YAML::Value << ac.Speed;
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

        out << YAML::Key << "CastShadows" << YAML::Value << mr.CastShadows;

        // Per-entity material property overrides
        if (!mr.MaterialOverrides.empty()) {
            out << YAML::Key << "MaterialOverrides" << YAML::Value << YAML::BeginSeq;
            for (const auto& ov : mr.MaterialOverrides) {
                out << YAML::BeginMap;
                out << YAML::Key << "Name" << YAML::Value << ov.Name;
                switch (ov.Type) {
                    case MaterialPropertyType::Float:
                        out << YAML::Key << "Type" << YAML::Value << "Float";
                        out << YAML::Key << "Value" << YAML::Value << ov.FloatValue;
                        if (ov.IsRange) {
                            out << YAML::Key << "RangeMin" << YAML::Value << ov.RangeMin;
                            out << YAML::Key << "RangeMax" << YAML::Value << ov.RangeMax;
                        }
                        break;
                    case MaterialPropertyType::Int:
                        out << YAML::Key << "Type" << YAML::Value << "Int";
                        out << YAML::Key << "Value" << YAML::Value << ov.IntValue;
                        break;
                    case MaterialPropertyType::Vec3:
                        out << YAML::Key << "Type" << YAML::Value << "Vec3";
                        out << YAML::Key << "Value" << YAML::Value << YAML::Flow
                            << YAML::BeginSeq << ov.Vec3Value.x << ov.Vec3Value.y << ov.Vec3Value.z << YAML::EndSeq;
                        break;
                    case MaterialPropertyType::Vec4:
                        out << YAML::Key << "Type" << YAML::Value << "Vec4";
                        out << YAML::Key << "Value" << YAML::Value << YAML::Flow
                            << YAML::BeginSeq << ov.Vec4Value.x << ov.Vec4Value.y << ov.Vec4Value.z << ov.Vec4Value.w << YAML::EndSeq;
                        break;
                    case MaterialPropertyType::Texture2D:
                        out << YAML::Key << "Type" << YAML::Value << "Texture2D";
                        out << YAML::Key << "Value" << YAML::Value << ov.TexturePath;
                        break;
                }
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;
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
    out << YAML::Key << "ShadowEnabled" << YAML::Value << ps.ShadowEnabled;
    out << YAML::Key << "ShadowBias" << YAML::Value << ps.ShadowBias;
    out << YAML::Key << "ShadowNormalBias" << YAML::Value << ps.ShadowNormalBias;
    out << YAML::Key << "ShadowPCFRadius" << YAML::Value << ps.ShadowPCFRadius;
    out << YAML::EndMap;

    out << YAML::Key << "Entities" << YAML::Value << YAML::BeginSeq;

    auto view = scene->GetAllEntitiesWith<IDComponent>();
    // entt iterates in reverse creation order; collect and reverse to preserve original order
    std::vector<entt::entity> entities(view.begin(), view.end());
    std::reverse(entities.begin(), entities.end());
    for (auto entityID : entities) {
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
        if (psNode["ShadowEnabled"]) ps.ShadowEnabled = psNode["ShadowEnabled"].as<bool>();
        if (psNode["ShadowBias"]) ps.ShadowBias = psNode["ShadowBias"].as<float>();
        if (psNode["ShadowNormalBias"]) ps.ShadowNormalBias = psNode["ShadowNormalBias"].as<float>();
        if (psNode["ShadowPCFRadius"]) ps.ShadowPCFRadius = psNode["ShadowPCFRadius"].as<int>();
    }

    auto entities = data["Entities"];
    if (!entities)
        return true;

    for (auto entityNode : entities) {
        uint64_t uuid = entityNode["Entity"].as<uint64_t>();

        std::string name = "GameObject";
        std::string goTag = "Untagged";
        int layer = 0;
        if (auto tagNode = entityNode["TagComponent"]) {
            name = tagNode["Tag"].as<std::string>();
            if (tagNode["GameObjectTag"]) goTag = tagNode["GameObjectTag"].as<std::string>();
            if (tagNode["Layer"]) layer = tagNode["Layer"].as<int>();
        }

        Entity entity = scene->CreateEntityWithUUID(UUID(uuid), name);
        auto& tc = entity.GetComponent<TagComponent>();
        tc.GameObjectTag = goTag;
        tc.Layer = layer;

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

        if (auto plNode = entityNode["PointLightComponent"]) {
            auto& pl = entity.AddComponent<PointLightComponent>();
            auto col = plNode["Color"];
            pl.Color = { col[0].as<float>(), col[1].as<float>(), col[2].as<float>() };
            pl.Intensity = plNode["Intensity"].as<float>();
            pl.Range = plNode["Range"].as<float>();
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

        // New separate collider components
        if (auto n = entityNode["BoxColliderComponent"]) {
            auto& col = entity.AddComponent<BoxColliderComponent>();
            auto sz = n["Size"];
            col.Size = { sz[0].as<float>(), sz[1].as<float>(), sz[2].as<float>() };
            auto off = n["Offset"];
            col.Offset = { off[0].as<float>(), off[1].as<float>(), off[2].as<float>() };
        }
        if (auto n = entityNode["SphereColliderComponent"]) {
            auto& col = entity.AddComponent<SphereColliderComponent>();
            col.Radius = n["Radius"].as<float>();
            auto off = n["Offset"];
            col.Offset = { off[0].as<float>(), off[1].as<float>(), off[2].as<float>() };
        }
        if (auto n = entityNode["CapsuleColliderComponent"]) {
            auto& col = entity.AddComponent<CapsuleColliderComponent>();
            col.Radius = n["Radius"].as<float>();
            col.Height = n["Height"].as<float>();
            auto off = n["Offset"];
            col.Offset = { off[0].as<float>(), off[1].as<float>(), off[2].as<float>() };
        }
        if (auto n = entityNode["MeshColliderComponent"]) {
            auto& col = entity.AddComponent<MeshColliderComponent>();
            col.Convex = n["Convex"].as<bool>(true);
            auto off = n["Offset"];
            col.Offset = { off[0].as<float>(), off[1].as<float>(), off[2].as<float>() };
        }
        // Backward compat: old ColliderComponent → convert to new type
        if (auto colNode = entityNode["ColliderComponent"]) {
            std::string sh = colNode["Shape"].as<std::string>("Box");
            auto sz = colNode["Size"];
            auto off = colNode["Offset"];
            if (sh == "Box") {
                auto& col = entity.AddComponent<BoxColliderComponent>();
                col.Size = { sz[0].as<float>(), sz[1].as<float>(), sz[2].as<float>() };
                col.Offset = { off[0].as<float>(), off[1].as<float>(), off[2].as<float>() };
            } else if (sh == "Sphere") {
                auto& col = entity.AddComponent<SphereColliderComponent>();
                col.Radius = sz[0].as<float>() * 0.5f;
                col.Offset = { off[0].as<float>(), off[1].as<float>(), off[2].as<float>() };
            } else {
                auto& col = entity.AddComponent<CapsuleColliderComponent>();
                col.Radius = sz[0].as<float>() * 0.5f;
                col.Height = sz[1].as<float>();
                col.Offset = { off[0].as<float>(), off[1].as<float>(), off[2].as<float>() };
            }
        }

        if (auto scNode = entityNode["ScriptComponent"]) {
            auto& sc = entity.AddComponent<ScriptComponent>();
            sc.ClassName = scNode["ClassName"].as<std::string>();
            if (auto propsNode = scNode["Properties"]) {
                for (auto it = propsNode.begin(); it != propsNode.end(); ++it) {
                    std::string key = it->first.as<std::string>();
                    auto& val = it->second;
                    // Try bool first (YAML bool is distinct), then int, then float
                    if (val.Tag() == "!" || val.Scalar() == "true" || val.Scalar() == "false") {
                        sc.Properties[key] = val.as<bool>();
                    } else {
                        // Try as float (covers both int and float in YAML)
                        try {
                            float f = val.as<float>();
                            // Check if it's actually an integer (no decimal point)
                            std::string s = val.Scalar();
                            if (s.find('.') == std::string::npos && s.find('e') == std::string::npos)
                                sc.Properties[key] = val.as<int>();
                            else
                                sc.Properties[key] = f;
                        } catch (...) {
                            sc.Properties[key] = 0.0f;
                        }
                    }
                }
            }
        }

        if (auto camNode = entityNode["CameraComponent"]) {
            auto& cam = entity.AddComponent<CameraComponent>();
            std::string pt = camNode["ProjectionType"].as<std::string>("Perspective");
            cam.ProjectionType = (pt == "Orthographic")
                ? CameraProjection::Orthographic : CameraProjection::Perspective;
            if (camNode["FOV"])      cam.FOV      = camNode["FOV"].as<float>();
            if (camNode["Size"])     cam.Size     = camNode["Size"].as<float>();
            if (camNode["NearClip"]) cam.NearClip = camNode["NearClip"].as<float>();
            if (camNode["FarClip"])  cam.FarClip  = camNode["FarClip"].as<float>();
            if (camNode["Priority"]) cam.Priority = camNode["Priority"].as<int>();
        }

        if (auto asNode = entityNode["AudioSourceComponent"]) {
            auto& as = entity.AddComponent<AudioSourceComponent>();
            if (asNode["ClipPath"])    as.ClipPath    = asNode["ClipPath"].as<std::string>();
            if (asNode["Volume"])      as.Volume      = asNode["Volume"].as<float>();
            if (asNode["Pitch"])       as.Pitch       = asNode["Pitch"].as<float>();
            if (asNode["Loop"])        as.Loop        = asNode["Loop"].as<bool>();
            if (asNode["Spatial"])     as.Spatial     = asNode["Spatial"].as<bool>();
            if (asNode["PlayOnAwake"]) as.PlayOnAwake = asNode["PlayOnAwake"].as<bool>();
            if (asNode["MinDistance"]) as.MinDistance = asNode["MinDistance"].as<float>();
            if (asNode["MaxDistance"]) as.MaxDistance = asNode["MaxDistance"].as<float>();
        }

        if (entityNode["AudioListenerComponent"]) {
            entity.AddComponent<AudioListenerComponent>();
        }

        if (auto srNode = entityNode["SpriteRendererComponent"]) {
            auto& sr = entity.AddComponent<SpriteRendererComponent>();
            if (auto c = srNode["Color"]) {
                sr.Color = { c[0].as<float>(), c[1].as<float>(), c[2].as<float>(), c[3].as<float>() };
            }
            if (srNode["TexturePath"]) {
                sr.TexturePath = srNode["TexturePath"].as<std::string>();
                if (!sr.TexturePath.empty())
                    sr.Texture = Texture2D::Create(sr.TexturePath);
            }
            if (auto uv = srNode["UVRect"]) {
                sr.UVRect = { uv[0].as<float>(), uv[1].as<float>(), uv[2].as<float>(), uv[3].as<float>() };
            }
            if (srNode["SortingOrder"]) sr.SortingOrder = srNode["SortingOrder"].as<int>();
        }

        if (auto saNode = entityNode["SpriteAnimatorComponent"]) {
            auto& sa = entity.AddComponent<SpriteAnimatorComponent>();
            if (saNode["Columns"])    sa.Columns    = saNode["Columns"].as<int>();
            if (saNode["Rows"])       sa.Rows       = saNode["Rows"].as<int>();
            if (saNode["StartFrame"]) sa.StartFrame = saNode["StartFrame"].as<int>();
            if (saNode["EndFrame"])   sa.EndFrame   = saNode["EndFrame"].as<int>();
            if (saNode["FrameRate"])  sa.FrameRate  = saNode["FrameRate"].as<float>();
            if (saNode["Loop"])       sa.Loop       = saNode["Loop"].as<bool>();
            if (saNode["PlayOnStart"]) sa.PlayOnStart = saNode["PlayOnStart"].as<bool>();
        }

        if (auto psNode = entityNode["ParticleSystemComponent"]) {
            auto& ps = entity.AddComponent<ParticleSystemComponent>();
            if (psNode["EmissionRate"])     ps.EmissionRate     = psNode["EmissionRate"].as<float>();
            if (psNode["ParticleLifetime"]) ps.ParticleLifetime = psNode["ParticleLifetime"].as<float>();
            if (psNode["LifetimeVariance"]) ps.LifetimeVariance = psNode["LifetimeVariance"].as<float>();
            if (psNode["MaxParticles"])     ps.MaxParticles     = psNode["MaxParticles"].as<int>();
            if (auto v = psNode["VelocityMin"])
                ps.VelocityMin = { v[0].as<float>(), v[1].as<float>(), v[2].as<float>() };
            if (auto v = psNode["VelocityMax"])
                ps.VelocityMax = { v[0].as<float>(), v[1].as<float>(), v[2].as<float>() };
            if (auto v = psNode["Gravity"])
                ps.Gravity = { v[0].as<float>(), v[1].as<float>(), v[2].as<float>() };
            if (auto c = psNode["StartColor"])
                ps.StartColor = { c[0].as<float>(), c[1].as<float>(), c[2].as<float>(), c[3].as<float>() };
            if (auto c = psNode["EndColor"])
                ps.EndColor = { c[0].as<float>(), c[1].as<float>(), c[2].as<float>(), c[3].as<float>() };
            if (psNode["StartSize"])  ps.StartSize  = psNode["StartSize"].as<float>();
            if (psNode["EndSize"])    ps.EndSize    = psNode["EndSize"].as<float>();
            if (psNode["TexturePath"]) {
                ps.TexturePath = psNode["TexturePath"].as<std::string>();
                if (!ps.TexturePath.empty())
                    ps.Texture = Texture2D::Create(ps.TexturePath);
            }
            if (psNode["PlayOnStart"]) ps.PlayOnStart = psNode["PlayOnStart"].as<bool>();
        }

        if (auto acNode = entityNode["AnimatorComponent"]) {
            auto& ac = entity.AddComponent<AnimatorComponent>();
            if (acNode["AnimationSource"]) ac.AnimationSourcePath = acNode["AnimationSource"].as<std::string>();
            if (acNode["ClipIndex"])   ac.ClipIndex   = acNode["ClipIndex"].as<int>();
            if (acNode["PlayOnStart"]) ac.PlayOnStart = acNode["PlayOnStart"].as<bool>();
            if (acNode["Loop"])        ac.Loop        = acNode["Loop"].as<bool>();
            if (acNode["Speed"])       ac.Speed       = acNode["Speed"].as<float>();
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
            if (auto castNode = mrNode["CastShadows"])
                mr.CastShadows = castNode.as<bool>();
            // Per-entity material property overrides
            if (auto ovNode = mrNode["MaterialOverrides"]) {
                for (auto propNode : ovNode) {
                    MaterialProperty ov;
                    ov.Name = propNode["Name"].as<std::string>();
                    std::string type = propNode["Type"].as<std::string>();
                    if (type == "Float") {
                        ov.Type = MaterialPropertyType::Float;
                        ov.FloatValue = propNode["Value"].as<float>();
                        if (propNode["RangeMin"]) {
                            ov.IsRange = true;
                            ov.RangeMin = propNode["RangeMin"].as<float>();
                            ov.RangeMax = propNode["RangeMax"].as<float>();
                        }
                    } else if (type == "Int") {
                        ov.Type = MaterialPropertyType::Int;
                        ov.IntValue = propNode["Value"].as<int>();
                    } else if (type == "Vec3") {
                        ov.Type = MaterialPropertyType::Vec3;
                        auto v = propNode["Value"];
                        ov.Vec3Value = { v[0].as<float>(), v[1].as<float>(), v[2].as<float>() };
                    } else if (type == "Vec4") {
                        ov.Type = MaterialPropertyType::Vec4;
                        auto v = propNode["Value"];
                        ov.Vec4Value = { v[0].as<float>(), v[1].as<float>(), v[2].as<float>(), v[3].as<float>() };
                    } else if (type == "Texture2D") {
                        ov.Type = MaterialPropertyType::Texture2D;
                        ov.TexturePath = propNode["Value"].as<std::string>();
                        if (!ov.TexturePath.empty())
                            ov.TextureRef = Texture2D::Create(ov.TexturePath);
                    }
                    // Restore display name from material if available
                    if (mr.Mat) {
                        for (const auto& mp : mr.Mat->GetProperties()) {
                            if (mp.Name == ov.Name) {
                                ov.DisplayName = mp.DisplayName;
                                if (mp.IsRange) {
                                    ov.IsRange = true;
                                    ov.RangeMin = mp.RangeMin;
                                    ov.RangeMax = mp.RangeMax;
                                }
                                break;
                            }
                        }
                    }
                    mr.MaterialOverrides.push_back(std::move(ov));
                }
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
