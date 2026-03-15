#include "VibeEngine/Scene/SceneSerializer.h"
#include "VibeEngine/Scene/Entity.h"
#include "VibeEngine/Scene/Components.h"
#include "VibeEngine/Scene/MeshLibrary.h"
#include "VibeEngine/Asset/MeshImporter.h"
#include "VibeEngine/Renderer/Material.h"
#include "VibeEngine/Renderer/LODSystem.h"
#include "VibeEngine/Core/Log.h"

#include <yaml-cpp/yaml.h>
#include <fstream>
#include <filesystem>

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
        if (!tc.Active)
            out << YAML::Key << "Active" << YAML::Value << false;
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
        out << YAML::Key << "CastShadows" << YAML::Value << pl.CastShadows;
        out << YAML::EndMap;
    }

    // SpotLightComponent
    if (entity.HasComponent<SpotLightComponent>()) {
        auto& sl = entity.GetComponent<SpotLightComponent>();
        out << YAML::Key << "SpotLightComponent" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Direction" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << sl.Direction[0] << sl.Direction[1] << sl.Direction[2] << YAML::EndSeq;
        out << YAML::Key << "Color" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << sl.Color[0] << sl.Color[1] << sl.Color[2] << YAML::EndSeq;
        out << YAML::Key << "Intensity" << YAML::Value << sl.Intensity;
        out << YAML::Key << "Range" << YAML::Value << sl.Range;
        out << YAML::Key << "InnerAngle" << YAML::Value << sl.InnerAngle;
        out << YAML::Key << "OuterAngle" << YAML::Value << sl.OuterAngle;
        out << YAML::Key << "CastShadows" << YAML::Value << sl.CastShadows;
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

    // VideoPlayerComponent
    if (entity.HasComponent<VideoPlayerComponent>()) {
        auto& vp = entity.GetComponent<VideoPlayerComponent>();
        out << YAML::Key << "VideoPlayerComponent" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "VideoPath" << YAML::Value << vp.VideoPath;
        out << YAML::Key << "PlayOnAwake" << YAML::Value << vp.PlayOnAwake;
        out << YAML::Key << "Loop" << YAML::Value << vp.Loop;
        out << YAML::Key << "Volume" << YAML::Value << vp.Volume;
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
        out << YAML::Key << "Looping" << YAML::Value << ps.Looping;
        out << YAML::Key << "EmitterShape" << YAML::Value << static_cast<int>(ps.Shape);
        out << YAML::Key << "ShapeRadius" << YAML::Value << ps.ShapeRadius;
        out << YAML::Key << "ConeAngle" << YAML::Value << ps.ConeAngle;
        out << YAML::Key << "SpeedMin" << YAML::Value << ps.SpeedMin;
        out << YAML::Key << "SpeedMax" << YAML::Value << ps.SpeedMax;
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

        // State machine
        out << YAML::Key << "UseStateMachine" << YAML::Value << ac.UseStateMachine;
        if (ac.UseStateMachine) {
            out << YAML::Key << "DefaultState" << YAML::Value << ac.DefaultState;

            out << YAML::Key << "States" << YAML::Value << YAML::BeginSeq;
            for (auto& s : ac.States) {
                out << YAML::BeginMap;
                out << YAML::Key << "Name" << YAML::Value << s.Name;
                out << YAML::Key << "Clip" << YAML::Value << s.ClipIndex;
                out << YAML::Key << "Speed" << YAML::Value << s.Speed;
                out << YAML::Key << "Loop" << YAML::Value << s.Loop;
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;

            out << YAML::Key << "Parameters" << YAML::Value << YAML::BeginSeq;
            for (auto& p : ac.Parameters) {
                out << YAML::BeginMap;
                out << YAML::Key << "Name" << YAML::Value << p.Name;
                out << YAML::Key << "Type" << YAML::Value << static_cast<int>(p.Type);
                out << YAML::Key << "Float" << YAML::Value << p.FloatValue;
                out << YAML::Key << "Int" << YAML::Value << p.IntValue;
                out << YAML::Key << "Bool" << YAML::Value << p.BoolValue;
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;

            out << YAML::Key << "Transitions" << YAML::Value << YAML::BeginSeq;
            for (auto& t : ac.Transitions) {
                out << YAML::BeginMap;
                out << YAML::Key << "From" << YAML::Value << t.FromState;
                out << YAML::Key << "To" << YAML::Value << t.ToState;
                out << YAML::Key << "Duration" << YAML::Value << t.Duration;
                out << YAML::Key << "HasExitTime" << YAML::Value << t.HasExitTime;
                out << YAML::Key << "ExitTime" << YAML::Value << t.ExitTime;
                out << YAML::Key << "Conditions" << YAML::Value << YAML::BeginSeq;
                for (auto& c : t.Conditions) {
                    out << YAML::BeginMap;
                    out << YAML::Key << "Param" << YAML::Value << c.ParamName;
                    out << YAML::Key << "Op" << YAML::Value << static_cast<int>(c.Op);
                    out << YAML::Key << "Threshold" << YAML::Value << c.Threshold;
                    out << YAML::EndMap;
                }
                out << YAML::EndSeq;
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;
        }

        out << YAML::EndMap;
    }

    // NavAgentComponent
    if (entity.HasComponent<NavAgentComponent>()) {
        auto& nav = entity.GetComponent<NavAgentComponent>();
        out << YAML::Key << "NavAgentComponent" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Speed" << YAML::Value << nav.Speed;
        out << YAML::Key << "StoppingDist" << YAML::Value << nav.StoppingDist;
        out << YAML::Key << "AgentRadius" << YAML::Value << nav.AgentRadius;
        out << YAML::EndMap;
    }

    // LODGroupComponent
    if (entity.HasComponent<LODGroupComponent>()) {
        auto& lod = entity.GetComponent<LODGroupComponent>();
        out << YAML::Key << "LODGroupComponent" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "CullDistance" << YAML::Value << lod.CullDistance;
        out << YAML::Key << "Levels" << YAML::Value << YAML::BeginSeq;
        for (const auto& level : lod.Levels) {
            out << YAML::BeginMap;
            out << YAML::Key << "MeshType" << YAML::Value << level.MeshType;
            if (!level.MeshSourcePath.empty())
                out << YAML::Key << "MeshSource" << YAML::Value << level.MeshSourcePath;
            out << YAML::Key << "MaxDistance" << YAML::Value << level.MaxDistance;
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;
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
        // Only serialize overrides that have actual user-set values
        // (skip Texture2D without a real file, skip defaults from auto-populate)
        {
            std::vector<const MaterialProperty*> effectiveOverrides;
            for (const auto& ov : mr.MaterialOverrides) {
                if (ov.Type == MaterialPropertyType::Texture2D) {
                    // Only save textures with actual file paths (not ShaderLab defaults)
                    if (ov.TextureRef && !ov.TexturePath.empty())
                        effectiveOverrides.push_back(&ov);
                } else {
                    effectiveOverrides.push_back(&ov);
                }
            }

            if (!effectiveOverrides.empty()) {
                out << YAML::Key << "MaterialOverrides" << YAML::Value << YAML::BeginSeq;
                for (const auto* ov : effectiveOverrides) {
                    out << YAML::BeginMap;
                    out << YAML::Key << "Name" << YAML::Value << ov->Name;
                    switch (ov->Type) {
                        case MaterialPropertyType::Float:
                            out << YAML::Key << "Type" << YAML::Value << "Float";
                            out << YAML::Key << "Value" << YAML::Value << ov->FloatValue;
                            if (ov->IsRange) {
                                out << YAML::Key << "RangeMin" << YAML::Value << ov->RangeMin;
                                out << YAML::Key << "RangeMax" << YAML::Value << ov->RangeMax;
                            }
                            break;
                        case MaterialPropertyType::Int:
                            out << YAML::Key << "Type" << YAML::Value << "Int";
                            out << YAML::Key << "Value" << YAML::Value << ov->IntValue;
                            break;
                        case MaterialPropertyType::Vec3:
                            out << YAML::Key << "Type" << YAML::Value << "Vec3";
                            out << YAML::Key << "Value" << YAML::Value << YAML::Flow
                                << YAML::BeginSeq << ov->Vec3Value.x << ov->Vec3Value.y << ov->Vec3Value.z << YAML::EndSeq;
                            break;
                        case MaterialPropertyType::Vec4:
                            out << YAML::Key << "Type" << YAML::Value << "Vec4";
                            out << YAML::Key << "Value" << YAML::Value << YAML::Flow
                                << YAML::BeginSeq << ov->Vec4Value.x << ov->Vec4Value.y << ov->Vec4Value.z << ov->Vec4Value.w << YAML::EndSeq;
                            break;
                        case MaterialPropertyType::Texture2D:
                            out << YAML::Key << "Type" << YAML::Value << "Texture2D";
                            out << YAML::Key << "Value" << YAML::Value << ov->TexturePath;
                            break;
                    }
                    out << YAML::EndMap;
                }
                out << YAML::EndSeq;
            }
        }

        out << YAML::EndMap;
    }

    // TerrainComponent
    if (entity.HasComponent<TerrainComponent>()) {
        auto& t = entity.GetComponent<TerrainComponent>();
        out << YAML::Key << "TerrainComponent" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Resolution" << YAML::Value << t.Resolution;
        out << YAML::Key << "WorldSizeX" << YAML::Value << t.WorldSizeX;
        out << YAML::Key << "WorldSizeZ" << YAML::Value << t.WorldSizeZ;
        out << YAML::Key << "HeightScale" << YAML::Value << t.HeightScale;
        if (!t.HeightmapPath.empty())
            out << YAML::Key << "HeightmapPath" << YAML::Value << t.HeightmapPath;
        out << YAML::Key << "Octaves" << YAML::Value << t.Octaves;
        out << YAML::Key << "Persistence" << YAML::Value << t.Persistence;
        out << YAML::Key << "Lacunarity" << YAML::Value << t.Lacunarity;
        out << YAML::Key << "NoiseScale" << YAML::Value << t.NoiseScale;
        out << YAML::Key << "Seed" << YAML::Value << t.Seed;
        out << YAML::Key << "LayerTextures" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << t.LayerTexturePaths[0] << t.LayerTexturePaths[1]
            << t.LayerTexturePaths[2] << t.LayerTexturePaths[3] << YAML::EndSeq;
        out << YAML::Key << "LayerTiling" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << t.LayerTiling[0] << t.LayerTiling[1]
            << t.LayerTiling[2] << t.LayerTiling[3] << YAML::EndSeq;
        out << YAML::Key << "BlendHeights" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << t.BlendHeights[0] << t.BlendHeights[1] << t.BlendHeights[2] << YAML::EndSeq;
        out << YAML::Key << "Roughness" << YAML::Value << t.Roughness;
        out << YAML::EndMap;
    }

    // UICanvasComponent
    if (entity.HasComponent<UICanvasComponent>()) {
        auto& uc = entity.GetComponent<UICanvasComponent>();
        out << YAML::Key << "UICanvasComponent" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "ScreenSpace" << YAML::Value << uc.ScreenSpace;
        out << YAML::Key << "SortOrder" << YAML::Value << uc.SortOrder;
        out << YAML::EndMap;
    }

    // UIRectTransformComponent
    if (entity.HasComponent<UIRectTransformComponent>()) {
        auto& rt = entity.GetComponent<UIRectTransformComponent>();
        out << YAML::Key << "UIRectTransformComponent" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Anchor" << YAML::Value << static_cast<int>(rt.Anchor);
        out << YAML::Key << "AnchoredPosition" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << rt.AnchoredPosition[0] << rt.AnchoredPosition[1] << YAML::EndSeq;
        out << YAML::Key << "Size" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << rt.Size[0] << rt.Size[1] << YAML::EndSeq;
        out << YAML::Key << "Pivot" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << rt.Pivot[0] << rt.Pivot[1] << YAML::EndSeq;
        out << YAML::EndMap;
    }

    // UITextComponent
    if (entity.HasComponent<UITextComponent>()) {
        auto& txt = entity.GetComponent<UITextComponent>();
        out << YAML::Key << "UITextComponent" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Text" << YAML::Value << txt.Text;
        out << YAML::Key << "FontSize" << YAML::Value << txt.FontSize;
        out << YAML::Key << "Color" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << txt.Color[0] << txt.Color[1] << txt.Color[2] << txt.Color[3] << YAML::EndSeq;
        if (!txt.FontPath.empty())
            out << YAML::Key << "FontPath" << YAML::Value << txt.FontPath;
        out << YAML::EndMap;
    }

    // UIImageComponent
    if (entity.HasComponent<UIImageComponent>()) {
        auto& img = entity.GetComponent<UIImageComponent>();
        out << YAML::Key << "UIImageComponent" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Color" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << img.Color[0] << img.Color[1] << img.Color[2] << img.Color[3] << YAML::EndSeq;
        if (!img.TexturePath.empty())
            out << YAML::Key << "TexturePath" << YAML::Value << img.TexturePath;
        out << YAML::EndMap;
    }

    // UIButtonComponent
    if (entity.HasComponent<UIButtonComponent>()) {
        auto& btn = entity.GetComponent<UIButtonComponent>();
        out << YAML::Key << "UIButtonComponent" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Label" << YAML::Value << btn.Label;
        out << YAML::Key << "FontSize" << YAML::Value << btn.FontSize;
        out << YAML::Key << "LabelColor" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << btn.LabelColor[0] << btn.LabelColor[1] << btn.LabelColor[2] << btn.LabelColor[3] << YAML::EndSeq;
        out << YAML::Key << "NormalColor" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << btn.NormalColor[0] << btn.NormalColor[1] << btn.NormalColor[2] << btn.NormalColor[3] << YAML::EndSeq;
        out << YAML::Key << "HoverColor" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << btn.HoverColor[0] << btn.HoverColor[1] << btn.HoverColor[2] << btn.HoverColor[3] << YAML::EndSeq;
        out << YAML::Key << "PressedColor" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << btn.PressedColor[0] << btn.PressedColor[1] << btn.PressedColor[2] << btn.PressedColor[3] << YAML::EndSeq;
        out << YAML::EndMap;
    }

    // DecalComponent
    if (entity.HasComponent<DecalComponent>()) {
        auto& dc = entity.GetComponent<DecalComponent>();
        out << YAML::Key << "DecalComponent" << YAML::Value << YAML::BeginMap;
        if (!dc.TexturePath.empty())
            out << YAML::Key << "TexturePath" << YAML::Value << dc.TexturePath;
        out << YAML::Key << "Color" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << dc.Color[0] << dc.Color[1] << dc.Color[2] << dc.Color[3] << YAML::EndSeq;
        out << YAML::Key << "Size" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << dc.Size[0] << dc.Size[1] << dc.Size[2] << YAML::EndSeq;
        out << YAML::Key << "NormalBlend" << YAML::Value << dc.NormalBlend;
        out << YAML::Key << "FadeDistance" << YAML::Value << dc.FadeDistance;
        out << YAML::Key << "SortOrder" << YAML::Value << dc.SortOrder;
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
    out << YAML::Key << "HDREnabled" << YAML::Value << ps.HDREnabled;
    out << YAML::Key << "ToneMapMode" << YAML::Value << ps.ToneMapMode;
    out << YAML::Key << "HDRExposure" << YAML::Value << ps.Exposure;
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
    out << YAML::Key << "FogEnabled" << YAML::Value << ps.FogEnabled;
    out << YAML::Key << "FogMode" << YAML::Value << ps.FogMode;
    out << YAML::Key << "FogColor" << YAML::Value << YAML::Flow
        << YAML::BeginSeq << ps.FogColor[0] << ps.FogColor[1] << ps.FogColor[2] << YAML::EndSeq;
    out << YAML::Key << "FogDensity" << YAML::Value << ps.FogDensity;
    out << YAML::Key << "FogStart" << YAML::Value << ps.FogStart;
    out << YAML::Key << "FogEnd" << YAML::Value << ps.FogEnd;
    out << YAML::Key << "FogHeightFalloff" << YAML::Value << ps.FogHeightFalloff;
    out << YAML::Key << "FogMaxOpacity" << YAML::Value << ps.FogMaxOpacity;
    out << YAML::Key << "VolFogEnabled" << YAML::Value << ps.VolFogEnabled;
    out << YAML::Key << "VolFogDensity" << YAML::Value << ps.VolFogDensity;
    out << YAML::Key << "VolFogScattering" << YAML::Value << ps.VolFogScattering;
    out << YAML::Key << "VolFogLightIntensity" << YAML::Value << ps.VolFogLightIntensity;
    out << YAML::Key << "VolFogColor" << YAML::Value << YAML::Flow
        << YAML::BeginSeq << ps.VolFogColor[0] << ps.VolFogColor[1] << ps.VolFogColor[2] << YAML::EndSeq;
    out << YAML::Key << "VolFogSteps" << YAML::Value << ps.VolFogSteps;
    out << YAML::Key << "VolFogMaxDistance" << YAML::Value << ps.VolFogMaxDistance;
    out << YAML::Key << "VolFogHeightFalloff" << YAML::Value << ps.VolFogHeightFalloff;
    out << YAML::Key << "VolFogBaseHeight" << YAML::Value << ps.VolFogBaseHeight;
    out << YAML::Key << "MotionBlurEnabled" << YAML::Value << ps.MotionBlurEnabled;
    out << YAML::Key << "MotionBlurStrength" << YAML::Value << ps.MotionBlurStrength;
    out << YAML::Key << "MotionBlurSamples" << YAML::Value << ps.MotionBlurSamples;
    out << YAML::Key << "SSAOEnabled" << YAML::Value << ps.SSAOEnabled;
    out << YAML::Key << "SSAORadius" << YAML::Value << ps.SSAORadius;
    out << YAML::Key << "SSAOBias" << YAML::Value << ps.SSAOBias;
    out << YAML::Key << "SSAOIntensity" << YAML::Value << ps.SSAOIntensity;
    out << YAML::Key << "SSAOKernelSize" << YAML::Value << ps.SSAOKernelSize;
    out << YAML::Key << "SSREnabled" << YAML::Value << ps.SSREnabled;
    out << YAML::Key << "SSRMaxSteps" << YAML::Value << ps.SSRMaxSteps;
    out << YAML::Key << "SSRStepSize" << YAML::Value << ps.SSRStepSize;
    out << YAML::Key << "SSRThickness" << YAML::Value << ps.SSRThickness;
    out << YAML::Key << "SSRMaxDistance" << YAML::Value << ps.SSRMaxDistance;
    out << YAML::Key << "DoFEnabled" << YAML::Value << ps.DoFEnabled;
    out << YAML::Key << "DoFFocusDistance" << YAML::Value << ps.DoFFocusDistance;
    out << YAML::Key << "DoFFocusRange" << YAML::Value << ps.DoFFocusRange;
    out << YAML::Key << "DoFMaxBlur" << YAML::Value << ps.DoFMaxBlur;
    out << YAML::Key << "DoFApertureSize" << YAML::Value << ps.DoFApertureSize;
    out << YAML::Key << "AAMode" << YAML::Value << ps.AAMode;
    out << YAML::Key << "FXAAEdgeThreshold" << YAML::Value << ps.FXAAEdgeThreshold;
    out << YAML::Key << "FXAAEdgeThresholdMin" << YAML::Value << ps.FXAAEdgeThresholdMin;
    out << YAML::Key << "FXAASubpixelQuality" << YAML::Value << ps.FXAASubpixelQuality;
    out << YAML::Key << "TAABlendFactor" << YAML::Value << ps.TAABlendFactor;
    out << YAML::Key << "ShadowEnabled" << YAML::Value << ps.ShadowEnabled;
    out << YAML::Key << "ShadowBias" << YAML::Value << ps.ShadowBias;
    out << YAML::Key << "ShadowNormalBias" << YAML::Value << ps.ShadowNormalBias;
    out << YAML::Key << "ShadowPCFRadius" << YAML::Value << ps.ShadowPCFRadius;
    out << YAML::Key << "OcclusionCullingEnabled" << YAML::Value << ps.OcclusionCullingEnabled;
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
        if (psNode["HDREnabled"]) ps.HDREnabled = psNode["HDREnabled"].as<bool>();
        if (psNode["ToneMapMode"]) ps.ToneMapMode = psNode["ToneMapMode"].as<int>();
        if (psNode["HDRExposure"]) ps.Exposure = psNode["HDRExposure"].as<float>();
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
        if (psNode["FogEnabled"]) ps.FogEnabled = psNode["FogEnabled"].as<bool>();
        if (psNode["FogMode"]) ps.FogMode = psNode["FogMode"].as<int>();
        if (auto fc = psNode["FogColor"])
            ps.FogColor = { fc[0].as<float>(), fc[1].as<float>(), fc[2].as<float>() };
        if (psNode["FogDensity"]) ps.FogDensity = psNode["FogDensity"].as<float>();
        if (psNode["FogStart"]) ps.FogStart = psNode["FogStart"].as<float>();
        if (psNode["FogEnd"]) ps.FogEnd = psNode["FogEnd"].as<float>();
        if (psNode["FogHeightFalloff"]) ps.FogHeightFalloff = psNode["FogHeightFalloff"].as<float>();
        if (psNode["FogMaxOpacity"]) ps.FogMaxOpacity = psNode["FogMaxOpacity"].as<float>();
        if (psNode["VolFogEnabled"]) ps.VolFogEnabled = psNode["VolFogEnabled"].as<bool>();
        if (psNode["VolFogDensity"]) ps.VolFogDensity = psNode["VolFogDensity"].as<float>();
        if (psNode["VolFogScattering"]) ps.VolFogScattering = psNode["VolFogScattering"].as<float>();
        if (psNode["VolFogLightIntensity"]) ps.VolFogLightIntensity = psNode["VolFogLightIntensity"].as<float>();
        if (auto vc = psNode["VolFogColor"])
            ps.VolFogColor = { vc[0].as<float>(), vc[1].as<float>(), vc[2].as<float>() };
        if (psNode["VolFogSteps"]) ps.VolFogSteps = psNode["VolFogSteps"].as<int>();
        if (psNode["VolFogMaxDistance"]) ps.VolFogMaxDistance = psNode["VolFogMaxDistance"].as<float>();
        if (psNode["VolFogHeightFalloff"]) ps.VolFogHeightFalloff = psNode["VolFogHeightFalloff"].as<float>();
        if (psNode["VolFogBaseHeight"]) ps.VolFogBaseHeight = psNode["VolFogBaseHeight"].as<float>();
        if (psNode["MotionBlurEnabled"]) ps.MotionBlurEnabled = psNode["MotionBlurEnabled"].as<bool>();
        if (psNode["MotionBlurStrength"]) ps.MotionBlurStrength = psNode["MotionBlurStrength"].as<float>();
        if (psNode["MotionBlurSamples"]) ps.MotionBlurSamples = psNode["MotionBlurSamples"].as<int>();
        if (psNode["SSAOEnabled"]) ps.SSAOEnabled = psNode["SSAOEnabled"].as<bool>();
        if (psNode["SSAORadius"]) ps.SSAORadius = psNode["SSAORadius"].as<float>();
        if (psNode["SSAOBias"]) ps.SSAOBias = psNode["SSAOBias"].as<float>();
        if (psNode["SSAOIntensity"]) ps.SSAOIntensity = psNode["SSAOIntensity"].as<float>();
        if (psNode["SSAOKernelSize"]) ps.SSAOKernelSize = psNode["SSAOKernelSize"].as<int>();
        if (psNode["SSREnabled"]) ps.SSREnabled = psNode["SSREnabled"].as<bool>();
        if (psNode["SSRMaxSteps"]) ps.SSRMaxSteps = psNode["SSRMaxSteps"].as<int>();
        if (psNode["SSRStepSize"]) ps.SSRStepSize = psNode["SSRStepSize"].as<float>();
        if (psNode["SSRThickness"]) ps.SSRThickness = psNode["SSRThickness"].as<float>();
        if (psNode["SSRMaxDistance"]) ps.SSRMaxDistance = psNode["SSRMaxDistance"].as<float>();
        if (psNode["DoFEnabled"]) ps.DoFEnabled = psNode["DoFEnabled"].as<bool>();
        if (psNode["DoFFocusDistance"]) ps.DoFFocusDistance = psNode["DoFFocusDistance"].as<float>();
        if (psNode["DoFFocusRange"]) ps.DoFFocusRange = psNode["DoFFocusRange"].as<float>();
        if (psNode["DoFMaxBlur"]) ps.DoFMaxBlur = psNode["DoFMaxBlur"].as<float>();
        if (psNode["DoFApertureSize"]) ps.DoFApertureSize = psNode["DoFApertureSize"].as<float>();
        if (psNode["AAMode"]) ps.AAMode = psNode["AAMode"].as<int>();
        if (psNode["FXAAEdgeThreshold"]) ps.FXAAEdgeThreshold = psNode["FXAAEdgeThreshold"].as<float>();
        if (psNode["FXAAEdgeThresholdMin"]) ps.FXAAEdgeThresholdMin = psNode["FXAAEdgeThresholdMin"].as<float>();
        if (psNode["FXAASubpixelQuality"]) ps.FXAASubpixelQuality = psNode["FXAASubpixelQuality"].as<float>();
        if (psNode["TAABlendFactor"]) ps.TAABlendFactor = psNode["TAABlendFactor"].as<float>();
        if (psNode["ShadowEnabled"]) ps.ShadowEnabled = psNode["ShadowEnabled"].as<bool>();
        if (psNode["ShadowBias"]) ps.ShadowBias = psNode["ShadowBias"].as<float>();
        if (psNode["ShadowNormalBias"]) ps.ShadowNormalBias = psNode["ShadowNormalBias"].as<float>();
        if (psNode["ShadowPCFRadius"]) ps.ShadowPCFRadius = psNode["ShadowPCFRadius"].as<int>();
        if (psNode["OcclusionCullingEnabled"]) ps.OcclusionCullingEnabled = psNode["OcclusionCullingEnabled"].as<bool>();
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

        bool active = true;
        if (auto tagNode = entityNode["TagComponent"]) {
            if (tagNode["Active"]) active = tagNode["Active"].as<bool>();
        }

        Entity entity = scene->CreateEntityWithUUID(UUID(uuid), name);
        auto& tc = entity.GetComponent<TagComponent>();
        tc.GameObjectTag = goTag;
        tc.Layer = layer;
        tc.Active = active;

        if (auto tcNode = entityNode["TransformComponent"]) {
            try {
                auto& tc = entity.GetComponent<TransformComponent>();
                auto pos = tcNode["Position"];
                tc.Position = { pos[0].as<float>(), pos[1].as<float>(), pos[2].as<float>() };
                auto rot = tcNode["Rotation"];
                tc.Rotation = { rot[0].as<float>(), rot[1].as<float>(), rot[2].as<float>() };
                auto scl = tcNode["Scale"];
                tc.Scale = { scl[0].as<float>(), scl[1].as<float>(), scl[2].as<float>() };
            } catch (const std::exception& e) {
                VE_ENGINE_WARN("Failed to deserialize TransformComponent: {}", e.what());
            }
        }

        if (auto dlNode = entityNode["DirectionalLightComponent"]) {
            try {
                auto& dl = entity.AddComponent<DirectionalLightComponent>();
                auto dir = dlNode["Direction"];
                dl.Direction = { dir[0].as<float>(), dir[1].as<float>(), dir[2].as<float>() };
                auto col = dlNode["Color"];
                dl.Color = { col[0].as<float>(), col[1].as<float>(), col[2].as<float>() };
                dl.Intensity = dlNode["Intensity"].as<float>();
            } catch (const std::exception& e) {
                VE_ENGINE_WARN("Failed to deserialize DirectionalLightComponent: {}", e.what());
            }
        }

        if (auto plNode = entityNode["PointLightComponent"]) {
            try {
                auto& pl = entity.AddComponent<PointLightComponent>();
                auto col = plNode["Color"];
                pl.Color = { col[0].as<float>(), col[1].as<float>(), col[2].as<float>() };
                pl.Intensity = plNode["Intensity"].as<float>();
                pl.Range = plNode["Range"].as<float>();
                if (plNode["CastShadows"]) pl.CastShadows = plNode["CastShadows"].as<bool>();
            } catch (const std::exception& e) {
                VE_ENGINE_WARN("Failed to deserialize PointLightComponent: {}", e.what());
            }
        }

        if (auto slNode = entityNode["SpotLightComponent"]) {
            try {
                auto& sl = entity.AddComponent<SpotLightComponent>();
                if (auto d = slNode["Direction"]) sl.Direction = { d[0].as<float>(), d[1].as<float>(), d[2].as<float>() };
                if (auto c = slNode["Color"]) sl.Color = { c[0].as<float>(), c[1].as<float>(), c[2].as<float>() };
                if (slNode["Intensity"]) sl.Intensity = slNode["Intensity"].as<float>();
                if (slNode["Range"]) sl.Range = slNode["Range"].as<float>();
                if (slNode["InnerAngle"]) sl.InnerAngle = slNode["InnerAngle"].as<float>();
                if (slNode["OuterAngle"]) sl.OuterAngle = slNode["OuterAngle"].as<float>();
                if (slNode["CastShadows"]) sl.CastShadows = slNode["CastShadows"].as<bool>();
            } catch (const std::exception& e) {
                VE_ENGINE_WARN("Failed to deserialize SpotLightComponent: {}", e.what());
            }
        }

        if (auto rbNode = entityNode["RigidbodyComponent"]) {
            try {
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
            } catch (const std::exception& e) {
                VE_ENGINE_WARN("Failed to deserialize RigidbodyComponent: {}", e.what());
            }
        }

        // New separate collider components
        if (auto n = entityNode["BoxColliderComponent"]) {
            try {
                auto& col = entity.AddComponent<BoxColliderComponent>();
                auto sz = n["Size"];
                col.Size = { sz[0].as<float>(), sz[1].as<float>(), sz[2].as<float>() };
                auto off = n["Offset"];
                col.Offset = { off[0].as<float>(), off[1].as<float>(), off[2].as<float>() };
            } catch (const std::exception& e) {
                VE_ENGINE_WARN("Failed to deserialize BoxColliderComponent: {}", e.what());
            }
        }
        if (auto n = entityNode["SphereColliderComponent"]) {
            try {
                auto& col = entity.AddComponent<SphereColliderComponent>();
                col.Radius = n["Radius"].as<float>();
                auto off = n["Offset"];
                col.Offset = { off[0].as<float>(), off[1].as<float>(), off[2].as<float>() };
            } catch (const std::exception& e) {
                VE_ENGINE_WARN("Failed to deserialize SphereColliderComponent: {}", e.what());
            }
        }
        if (auto n = entityNode["CapsuleColliderComponent"]) {
            try {
                auto& col = entity.AddComponent<CapsuleColliderComponent>();
                col.Radius = n["Radius"].as<float>();
                col.Height = n["Height"].as<float>();
                auto off = n["Offset"];
                col.Offset = { off[0].as<float>(), off[1].as<float>(), off[2].as<float>() };
            } catch (const std::exception& e) {
                VE_ENGINE_WARN("Failed to deserialize CapsuleColliderComponent: {}", e.what());
            }
        }
        if (auto n = entityNode["MeshColliderComponent"]) {
            try {
                auto& col = entity.AddComponent<MeshColliderComponent>();
                col.Convex = n["Convex"].as<bool>(true);
                auto off = n["Offset"];
                col.Offset = { off[0].as<float>(), off[1].as<float>(), off[2].as<float>() };
            } catch (const std::exception& e) {
                VE_ENGINE_WARN("Failed to deserialize MeshColliderComponent: {}", e.what());
            }
        }
        // Backward compat: old ColliderComponent -> convert to new type
        if (auto colNode = entityNode["ColliderComponent"]) {
            try {
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
            } catch (const std::exception& e) {
                VE_ENGINE_WARN("Failed to deserialize ColliderComponent: {}", e.what());
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

        if (auto vpNode = entityNode["VideoPlayerComponent"]) {
            auto& vp = entity.AddComponent<VideoPlayerComponent>();
            if (vpNode["VideoPath"])   vp.VideoPath   = vpNode["VideoPath"].as<std::string>();
            if (vpNode["PlayOnAwake"]) vp.PlayOnAwake = vpNode["PlayOnAwake"].as<bool>();
            if (vpNode["Loop"])        vp.Loop        = vpNode["Loop"].as<bool>();
            if (vpNode["Volume"])      vp.Volume      = vpNode["Volume"].as<float>();
        }

        if (auto srNode = entityNode["SpriteRendererComponent"]) {
            try {
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
            } catch (const std::exception& e) {
                VE_ENGINE_WARN("Failed to deserialize SpriteRendererComponent: {}", e.what());
            }
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
            try {
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
                if (psNode["Looping"])     ps.Looping     = psNode["Looping"].as<bool>();
                if (psNode["EmitterShape"]) ps.Shape = static_cast<EmitterShape>(psNode["EmitterShape"].as<int>());
                if (psNode["ShapeRadius"]) ps.ShapeRadius = psNode["ShapeRadius"].as<float>();
                if (psNode["ConeAngle"])   ps.ConeAngle   = psNode["ConeAngle"].as<float>();
                if (psNode["SpeedMin"])    ps.SpeedMin    = psNode["SpeedMin"].as<float>();
                if (psNode["SpeedMax"])    ps.SpeedMax    = psNode["SpeedMax"].as<float>();
            } catch (const std::exception& e) {
                VE_ENGINE_WARN("Failed to deserialize ParticleSystemComponent: {}", e.what());
            }
        }

        if (auto acNode = entityNode["AnimatorComponent"]) {
            try {
                auto& ac = entity.AddComponent<AnimatorComponent>();
                if (acNode["AnimationSource"]) ac.AnimationSourcePath = acNode["AnimationSource"].as<std::string>();
                if (acNode["ClipIndex"])   ac.ClipIndex   = acNode["ClipIndex"].as<int>();
                if (acNode["PlayOnStart"]) ac.PlayOnStart = acNode["PlayOnStart"].as<bool>();
                if (acNode["Loop"])        ac.Loop        = acNode["Loop"].as<bool>();
                if (acNode["Speed"])       ac.Speed       = acNode["Speed"].as<float>();

                if (acNode["UseStateMachine"]) ac.UseStateMachine = acNode["UseStateMachine"].as<bool>();
                if (acNode["DefaultState"])    ac.DefaultState    = acNode["DefaultState"].as<int>();

                if (auto statesNode = acNode["States"]) {
                    for (auto sn : statesNode) {
                        AnimState s;
                        s.Name = sn["Name"].as<std::string>("State");
                        s.ClipIndex = sn["Clip"].as<int>(0);
                        s.Speed = sn["Speed"].as<float>(1.0f);
                        s.Loop = sn["Loop"].as<bool>(true);
                        ac.States.push_back(s);
                    }
                }
                if (auto paramsNode = acNode["Parameters"]) {
                    for (auto pn : paramsNode) {
                        AnimParameter p;
                        p.Name = pn["Name"].as<std::string>();
                        p.Type = static_cast<AnimParamType>(pn["Type"].as<int>(0));
                        p.FloatValue = pn["Float"].as<float>(0.0f);
                        p.IntValue = pn["Int"].as<int>(0);
                        p.BoolValue = pn["Bool"].as<bool>(false);
                        ac.Parameters.push_back(p);
                    }
                }
                if (auto transNode = acNode["Transitions"]) {
                    for (auto tn : transNode) {
                        AnimTransition t;
                        t.FromState = tn["From"].as<int>(-1);
                        t.ToState = tn["To"].as<int>(0);
                        t.Duration = tn["Duration"].as<float>(0.2f);
                        t.HasExitTime = tn["HasExitTime"].as<bool>(false);
                        t.ExitTime = tn["ExitTime"].as<float>(1.0f);
                        if (auto condsNode = tn["Conditions"]) {
                            for (auto cn : condsNode) {
                                AnimCondition c;
                                c.ParamName = cn["Param"].as<std::string>();
                                c.Op = static_cast<AnimConditionOp>(cn["Op"].as<int>(0));
                                c.Threshold = cn["Threshold"].as<float>(0.0f);
                                t.Conditions.push_back(c);
                            }
                        }
                        ac.Transitions.push_back(t);
                    }
                }
            } catch (const std::exception& e) {
                VE_ENGINE_WARN("Failed to deserialize AnimatorComponent: {}", e.what());
            }
        }

        if (auto navNode = entityNode["NavAgentComponent"]) {
            try {
                auto& nav = entity.AddComponent<NavAgentComponent>();
                if (navNode["Speed"])        nav.Speed        = navNode["Speed"].as<float>();
                if (navNode["StoppingDist"]) nav.StoppingDist = navNode["StoppingDist"].as<float>();
                if (navNode["AgentRadius"])  nav.AgentRadius  = navNode["AgentRadius"].as<float>();
            } catch (const std::exception& e) {
                VE_ENGINE_WARN("Failed to deserialize NavAgentComponent: {}", e.what());
            }
        }

        if (auto lodNode = entityNode["LODGroupComponent"]) {
            try {
                auto& lod = entity.AddComponent<LODGroupComponent>();
                if (lodNode["CullDistance"])
                    lod.CullDistance = lodNode["CullDistance"].as<float>();
                if (auto levelsNode = lodNode["Levels"]) {
                    for (auto levelNode : levelsNode) {
                        LODLevel level;
                        level.MeshType = levelNode["MeshType"].as<int>(-1);
                        if (level.MeshType >= 0 && level.MeshType < MeshLibrary::GetMeshCount()) {
                            level.Mesh = MeshLibrary::GetMeshByIndex(level.MeshType);
                        } else if (levelNode["MeshSource"]) {
                            level.MeshSourcePath = levelNode["MeshSource"].as<std::string>();
                            auto meshAsset = MeshImporter::GetOrLoad(level.MeshSourcePath);
                            if (meshAsset && meshAsset->VAO)
                                level.Mesh = meshAsset->VAO;
                        }
                        level.MaxDistance = levelNode["MaxDistance"].as<float>(50.0f);
                        lod.Levels.push_back(level);
                    }
                }
            } catch (const std::exception& e) {
                VE_ENGINE_WARN("Failed to deserialize LODGroupComponent: {}", e.what());
            }
        }

        if (auto mrNode = entityNode["MeshRendererComponent"]) {
          try {
            auto& mr = entity.AddComponent<MeshRendererComponent>();
            int meshIndex = mrNode["MeshType"].as<int>();
            if (meshIndex >= 0 && meshIndex < MeshLibrary::GetMeshCount()) {
                mr.Mesh = MeshLibrary::GetMeshByIndex(meshIndex);
                mr.Mat = MeshLibrary::IsLitMesh(meshIndex)
                    ? MaterialLibrary::Get("Lit")
                    : MaterialLibrary::Get("Default");
                mr.LocalBounds = MeshLibrary::GetMeshAABB(meshIndex);
            } else if (meshIndex == -1 && mrNode["MeshSource"]) {
                mr.MeshSourcePath = mrNode["MeshSource"].as<std::string>();
                auto meshAsset = MeshImporter::GetOrLoad(mr.MeshSourcePath);
                if (meshAsset && meshAsset->VAO) {
                    mr.Mesh = meshAsset->VAO;
                    mr.Mat = MaterialLibrary::Get("Lit");
                    mr.LocalBounds = meshAsset->BoundingBox;
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
            // Ensure lit meshes use lit material (fix for old scenes that saved
            // Sphere with "Default" unlit material before the vertex layout fix)
            if (meshIndex >= 0 && MeshLibrary::IsLitMesh(meshIndex) && mr.Mat) {
                if (mr.Mat->GetName() == "Default")
                    mr.Mat = MaterialLibrary::Get("Lit");
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
                        // Skip ShaderLab default texture names — they are not real file paths
                        if (!ov.TexturePath.empty() &&
                            ov.TexturePath != "white" && ov.TexturePath != "black" &&
                            ov.TexturePath != "bump" && ov.TexturePath != "gray")
                            ov.TextureRef = Texture2D::Create(ov.TexturePath);
                        else
                            ov.TexturePath.clear(); // clear the non-file default name
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
            // Backward compat: old TexturePath field -> set as material texture
            if (auto texNode = mrNode["TexturePath"]) {
                std::string texPath = texNode.as<std::string>();
                if (!texPath.empty() && mr.Mat)
                    mr.Mat->SetTexture("u_Texture", texPath);
            }
          } catch (const std::exception& e) {
            VE_ENGINE_WARN("Failed to deserialize MeshRendererComponent: {}", e.what());
          }
        }

        // TerrainComponent
        if (auto tNode = entityNode["TerrainComponent"]) {
            try {
                auto& t = entity.AddComponent<TerrainComponent>();
                if (tNode["Resolution"])   t.Resolution   = tNode["Resolution"].as<int>();
                if (tNode["WorldSizeX"])   t.WorldSizeX   = tNode["WorldSizeX"].as<float>();
                if (tNode["WorldSizeZ"])   t.WorldSizeZ   = tNode["WorldSizeZ"].as<float>();
                if (tNode["HeightScale"])  t.HeightScale  = tNode["HeightScale"].as<float>();
                if (tNode["HeightmapPath"]) t.HeightmapPath = tNode["HeightmapPath"].as<std::string>();
                if (tNode["Octaves"])      t.Octaves      = tNode["Octaves"].as<int>();
                if (tNode["Persistence"])  t.Persistence  = tNode["Persistence"].as<float>();
                if (tNode["Lacunarity"])   t.Lacunarity   = tNode["Lacunarity"].as<float>();
                if (tNode["NoiseScale"])   t.NoiseScale   = tNode["NoiseScale"].as<float>();
                if (tNode["Seed"])         t.Seed         = tNode["Seed"].as<int>();
                if (auto lt = tNode["LayerTextures"]) {
                    for (int i = 0; i < 4 && i < (int)lt.size(); i++)
                        t.LayerTexturePaths[i] = lt[i].as<std::string>();
                }
                if (auto tl = tNode["LayerTiling"]) {
                    for (int i = 0; i < 4 && i < (int)tl.size(); i++)
                        t.LayerTiling[i] = tl[i].as<float>();
                }
                if (auto bh = tNode["BlendHeights"]) {
                    for (int i = 0; i < 3 && i < (int)bh.size(); i++)
                        t.BlendHeights[i] = bh[i].as<float>();
                }
                if (tNode["Roughness"]) t.Roughness = tNode["Roughness"].as<float>();
                t._NeedsRebuild = true;
            } catch (const std::exception& e) {
                VE_ENGINE_WARN("Failed to deserialize TerrainComponent: {}", e.what());
            }
        }

        // UI Components
        if (auto ucNode = entityNode["UICanvasComponent"]) {
            auto& uc = entity.AddComponent<UICanvasComponent>();
            if (ucNode["ScreenSpace"]) uc.ScreenSpace = ucNode["ScreenSpace"].as<bool>();
            if (ucNode["SortOrder"])   uc.SortOrder   = ucNode["SortOrder"].as<int>();
        }

        if (auto rtNode = entityNode["UIRectTransformComponent"]) {
            try {
                auto& rt = entity.AddComponent<UIRectTransformComponent>();
                if (rtNode["Anchor"]) rt.Anchor = static_cast<UIAnchorType>(rtNode["Anchor"].as<int>());
                if (auto ap = rtNode["AnchoredPosition"])
                    rt.AnchoredPosition = { ap[0].as<float>(), ap[1].as<float>() };
                if (auto sz = rtNode["Size"])
                    rt.Size = { sz[0].as<float>(), sz[1].as<float>() };
                if (auto pv = rtNode["Pivot"])
                    rt.Pivot = { pv[0].as<float>(), pv[1].as<float>() };
            } catch (const std::exception& e) {
                VE_ENGINE_WARN("Failed to deserialize UIRectTransformComponent: {}", e.what());
            }
        }

        if (auto txtNode = entityNode["UITextComponent"]) {
            try {
                auto& txt = entity.AddComponent<UITextComponent>();
                if (txtNode["Text"])     txt.Text     = txtNode["Text"].as<std::string>();
                if (txtNode["FontSize"]) txt.FontSize = txtNode["FontSize"].as<float>();
                if (auto c = txtNode["Color"])
                    txt.Color = { c[0].as<float>(), c[1].as<float>(), c[2].as<float>(), c[3].as<float>() };
                if (txtNode["FontPath"]) txt.FontPath = txtNode["FontPath"].as<std::string>();
            } catch (const std::exception& e) {
                VE_ENGINE_WARN("Failed to deserialize UITextComponent: {}", e.what());
            }
        }

        if (auto imgNode = entityNode["UIImageComponent"]) {
            try {
                auto& img = entity.AddComponent<UIImageComponent>();
                if (auto c = imgNode["Color"])
                    img.Color = { c[0].as<float>(), c[1].as<float>(), c[2].as<float>(), c[3].as<float>() };
                if (imgNode["TexturePath"]) {
                    img.TexturePath = imgNode["TexturePath"].as<std::string>();
                    if (!img.TexturePath.empty())
                        img._Texture = Texture2D::Create(img.TexturePath);
                }
            } catch (const std::exception& e) {
                VE_ENGINE_WARN("Failed to deserialize UIImageComponent: {}", e.what());
            }
        }

        if (auto btnNode = entityNode["UIButtonComponent"]) {
            try {
                auto& btn = entity.AddComponent<UIButtonComponent>();
                if (btnNode["Label"])    btn.Label    = btnNode["Label"].as<std::string>();
                if (btnNode["FontSize"]) btn.FontSize = btnNode["FontSize"].as<float>();
                if (auto c = btnNode["LabelColor"])
                    btn.LabelColor = { c[0].as<float>(), c[1].as<float>(), c[2].as<float>(), c[3].as<float>() };
                if (auto c = btnNode["NormalColor"])
                    btn.NormalColor = { c[0].as<float>(), c[1].as<float>(), c[2].as<float>(), c[3].as<float>() };
                if (auto c = btnNode["HoverColor"])
                    btn.HoverColor = { c[0].as<float>(), c[1].as<float>(), c[2].as<float>(), c[3].as<float>() };
                if (auto c = btnNode["PressedColor"])
                    btn.PressedColor = { c[0].as<float>(), c[1].as<float>(), c[2].as<float>(), c[3].as<float>() };
            } catch (const std::exception& e) {
                VE_ENGINE_WARN("Failed to deserialize UIButtonComponent: {}", e.what());
            }
        }

        if (auto dcNode = entityNode["DecalComponent"]) {
            try {
                auto& dc = entity.AddComponent<DecalComponent>();
                if (dcNode["TexturePath"]) {
                    dc.TexturePath = dcNode["TexturePath"].as<std::string>();
                    if (!dc.TexturePath.empty())
                        dc._Texture = Texture2D::Create(dc.TexturePath);
                }
                if (auto c = dcNode["Color"])
                    dc.Color = { c[0].as<float>(), c[1].as<float>(), c[2].as<float>(), c[3].as<float>() };
                if (auto s = dcNode["Size"])
                    dc.Size = { s[0].as<float>(), s[1].as<float>(), s[2].as<float>() };
                if (dcNode["NormalBlend"])  dc.NormalBlend  = dcNode["NormalBlend"].as<float>();
                if (dcNode["FadeDistance"]) dc.FadeDistance = dcNode["FadeDistance"].as<float>();
                if (dcNode["SortOrder"])    dc.SortOrder    = dcNode["SortOrder"].as<int>();
            } catch (const std::exception& e) {
                VE_ENGINE_WARN("Failed to deserialize DecalComponent: {}", e.what());
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
    if (filepath.empty()) {
        VE_ENGINE_ERROR("SceneSerializer::Serialize: filepath is empty");
        return;
    }

    // Ensure parent directory exists (create if needed)
    auto parentDir = std::filesystem::path(filepath).parent_path();
    if (!parentDir.empty() && !std::filesystem::exists(parentDir)) {
        std::error_code ec;
        std::filesystem::create_directories(parentDir, ec);
        if (ec) {
            VE_ENGINE_ERROR("SceneSerializer::Serialize: failed to create directory '{0}': {1}",
                            parentDir.string(), ec.message());
            return;
        }
    }

    std::string yaml = SerializeSceneToYAML(m_Scene);
    std::ofstream fout(filepath);
    fout << yaml;
    fout.close();
    VE_ENGINE_INFO("Scene saved to: {0}", filepath);
}

// ── Deserialize ────────────────────────────────────────────────────

bool SceneSerializer::Deserialize(const std::string& filepath) {
    if (filepath.empty()) {
        VE_ENGINE_ERROR("SceneSerializer::Deserialize: filepath is empty");
        return false;
    }
    if (!std::filesystem::exists(filepath)) {
        VE_ENGINE_ERROR("SceneSerializer::Deserialize: file not found: {0}", filepath);
        return false;
    }

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

// ── Prefab System ─────────────────────────────────────────────────

static void CollectEntityHierarchy(entt::entity entity, entt::registry& reg,
                                    std::vector<entt::entity>& out) {
    out.push_back(entity);
    if (reg.all_of<RelationshipComponent>(entity)) {
        auto& rel = reg.get<RelationshipComponent>(entity);
        for (auto child : rel.Children) {
            if (reg.valid(child))
                CollectEntityHierarchy(child, reg, out);
        }
    }
}

void SceneSerializer::SerializePrefab(const std::string& filepath,
                                       Entity rootEntity, Scene& scene) {
    auto& reg = scene.GetRegistry();
    if (!rootEntity.IsValid()) return;

    // Collect root + all descendants
    std::vector<entt::entity> entities;
    CollectEntityHierarchy(rootEntity.GetHandle(), reg, entities);

    // Serialize
    YAML::Emitter out;
    out << YAML::BeginMap;

    auto& tag = rootEntity.GetComponent<TagComponent>();
    out << YAML::Key << "Prefab" << YAML::Value << tag.Tag;

    out << YAML::Key << "Entities" << YAML::Value << YAML::BeginSeq;
    for (auto e : entities) {
        Entity entity(e, &scene);
        SerializeEntity(out, entity, reg);
    }
    out << YAML::EndSeq;
    out << YAML::EndMap;

    std::ofstream fout(filepath);
    if (fout) {
        fout << out.c_str();
        VE_ENGINE_INFO("Prefab saved: {}", filepath);
    }
}

Entity SceneSerializer::InstantiatePrefab(const std::string& filepath, Scene& scene) {
    std::ifstream fin(filepath);
    if (!fin.is_open()) {
        VE_ENGINE_ERROR("Failed to open prefab: {}", filepath);
        return {};
    }

    YAML::Node data;
    try {
        data = YAML::Load(fin);
    } catch (...) {
        VE_ENGINE_ERROR("Failed to parse prefab: {}", filepath);
        return {};
    }

    if (!data["Prefab"] || !data["Entities"]) {
        VE_ENGINE_ERROR("Invalid prefab format: {}", filepath);
        return {};
    }

    auto& reg = scene.GetRegistry();

    // First pass: create entities with new UUIDs, build remap table
    std::unordered_map<uint64_t, uint64_t> uuidRemap; // old -> new
    std::unordered_map<uint64_t, entt::entity> newEntityMap; // new uuid -> handle
    std::unordered_map<uint64_t, uint64_t> parentMap; // new uuid -> old parent uuid

    Entity rootEntity;

    for (auto entityNode : data["Entities"]) {
        uint64_t oldUUID = entityNode["Entity"].as<uint64_t>();
        UUID newUUID; // generate fresh
        uuidRemap[oldUUID] = static_cast<uint64_t>(newUUID);

        std::string name = "Entity";
        std::string goTag = "Untagged";
        int layer = 0;
        bool active = true;
        if (auto tagNode = entityNode["TagComponent"]) {
            name = tagNode["Tag"].as<std::string>("Entity");
            if (tagNode["GameObjectTag"]) goTag = tagNode["GameObjectTag"].as<std::string>();
            if (tagNode["Layer"]) layer = tagNode["Layer"].as<int>();
            if (tagNode["Active"]) active = tagNode["Active"].as<bool>();
        }

        Entity entity = scene.CreateEntityWithUUID(newUUID, name);
        auto& tc = entity.GetComponent<TagComponent>();
        tc.GameObjectTag = goTag;
        tc.Layer = layer;
        tc.Active = active;

        newEntityMap[static_cast<uint64_t>(newUUID)] = entity.GetHandle();

        // Track parent reference
        if (entityNode["Parent"]) {
            parentMap[static_cast<uint64_t>(newUUID)] = entityNode["Parent"].as<uint64_t>();
        }

        // Deserialize transform
        if (auto tcNode = entityNode["TransformComponent"]) {
            auto& t = entity.GetComponent<TransformComponent>();
            auto pos = tcNode["Position"];
            t.Position = { pos[0].as<float>(), pos[1].as<float>(), pos[2].as<float>() };
            auto rot = tcNode["Rotation"];
            t.Rotation = { rot[0].as<float>(), rot[1].as<float>(), rot[2].as<float>() };
            auto scl = tcNode["Scale"];
            t.Scale = { scl[0].as<float>(), scl[1].as<float>(), scl[2].as<float>() };
        }

        // MeshRendererComponent
        if (auto mrNode = entityNode["MeshRendererComponent"]) {
            auto& mr = entity.AddComponent<MeshRendererComponent>();
            int meshIndex = mrNode["MeshType"].as<int>();
            if (meshIndex >= 0 && meshIndex < MeshLibrary::GetMeshCount()) {
                mr.Mesh = MeshLibrary::GetMeshByIndex(meshIndex);
                mr.Mat = MeshLibrary::IsLitMesh(meshIndex)
                    ? MaterialLibrary::Get("Lit") : MaterialLibrary::Get("Default");
                mr.LocalBounds = MeshLibrary::GetMeshAABB(meshIndex);
            } else if (meshIndex == -1 && mrNode["MeshSource"]) {
                mr.MeshSourcePath = mrNode["MeshSource"].as<std::string>();
                auto meshAsset = MeshImporter::GetOrLoad(mr.MeshSourcePath);
                if (meshAsset && meshAsset->VAO) {
                    mr.Mesh = meshAsset->VAO;
                    mr.Mat = MaterialLibrary::Get("Lit");
                    mr.LocalBounds = meshAsset->BoundingBox;
                }
            }
            if (auto colorNode = mrNode["Color"])
                mr.Color = { colorNode[0].as<float>(), colorNode[1].as<float>(),
                             colorNode[2].as<float>(), colorNode[3].as<float>() };
            if (auto matNameNode = mrNode["MaterialName"]) {
                auto mat = MaterialLibrary::Get(matNameNode.as<std::string>());
                if (mat) mr.Mat = mat;
            }
            if (auto castNode = mrNode["CastShadows"])
                mr.CastShadows = castNode.as<bool>();
        }

        // RigidbodyComponent
        if (auto rbNode = entityNode["RigidbodyComponent"]) {
            auto& rb = entity.AddComponent<RigidbodyComponent>();
            std::string bt = rbNode["BodyType"].as<std::string>("Dynamic");
            rb.Type = (bt == "Static") ? BodyType::Static :
                      (bt == "Kinematic") ? BodyType::Kinematic : BodyType::Dynamic;
            if (rbNode["Mass"]) rb.Mass = rbNode["Mass"].as<float>();
            if (rbNode["LinearDamping"]) rb.LinearDamping = rbNode["LinearDamping"].as<float>();
            if (rbNode["AngularDamping"]) rb.AngularDamping = rbNode["AngularDamping"].as<float>();
            if (rbNode["Restitution"]) rb.Restitution = rbNode["Restitution"].as<float>();
            if (rbNode["Friction"]) rb.Friction = rbNode["Friction"].as<float>();
            if (rbNode["UseGravity"]) rb.UseGravity = rbNode["UseGravity"].as<bool>();
        }

        // Colliders
        if (auto bcNode = entityNode["BoxColliderComponent"]) {
            auto& bc = entity.AddComponent<BoxColliderComponent>();
            if (auto s = bcNode["Size"]) bc.Size = { s[0].as<float>(), s[1].as<float>(), s[2].as<float>() };
            if (auto o = bcNode["Offset"]) bc.Offset = { o[0].as<float>(), o[1].as<float>(), o[2].as<float>() };
        }
        if (auto scNode = entityNode["SphereColliderComponent"]) {
            auto& sc = entity.AddComponent<SphereColliderComponent>();
            if (scNode["Radius"]) sc.Radius = scNode["Radius"].as<float>();
        }

        // ScriptComponent
        if (auto scrNode = entityNode["ScriptComponent"]) {
            auto& sc = entity.AddComponent<ScriptComponent>();
            sc.ClassName = scrNode["ClassName"].as<std::string>("");
        }

        // DirectionalLightComponent
        if (auto dlNode = entityNode["DirectionalLightComponent"]) {
            auto& dl = entity.AddComponent<DirectionalLightComponent>();
            if (auto d = dlNode["Direction"]) dl.Direction = { d[0].as<float>(), d[1].as<float>(), d[2].as<float>() };
            if (auto c = dlNode["Color"]) dl.Color = { c[0].as<float>(), c[1].as<float>(), c[2].as<float>() };
            if (dlNode["Intensity"]) dl.Intensity = dlNode["Intensity"].as<float>();
        }

        // PointLightComponent
        if (auto plNode = entityNode["PointLightComponent"]) {
            auto& pl = entity.AddComponent<PointLightComponent>();
            if (auto c = plNode["Color"]) pl.Color = { c[0].as<float>(), c[1].as<float>(), c[2].as<float>() };
            if (plNode["Intensity"]) pl.Intensity = plNode["Intensity"].as<float>();
            if (plNode["Range"]) pl.Range = plNode["Range"].as<float>();
            if (plNode["CastShadows"]) pl.CastShadows = plNode["CastShadows"].as<bool>();
        }

        // SpotLightComponent
        if (auto slNode = entityNode["SpotLightComponent"]) {
            auto& sl = entity.AddComponent<SpotLightComponent>();
            if (auto d = slNode["Direction"]) sl.Direction = { d[0].as<float>(), d[1].as<float>(), d[2].as<float>() };
            if (auto c = slNode["Color"]) sl.Color = { c[0].as<float>(), c[1].as<float>(), c[2].as<float>() };
            if (slNode["Intensity"]) sl.Intensity = slNode["Intensity"].as<float>();
            if (slNode["Range"]) sl.Range = slNode["Range"].as<float>();
            if (slNode["InnerAngle"]) sl.InnerAngle = slNode["InnerAngle"].as<float>();
            if (slNode["OuterAngle"]) sl.OuterAngle = slNode["OuterAngle"].as<float>();
            if (slNode["CastShadows"]) sl.CastShadows = slNode["CastShadows"].as<bool>();
        }

        // SpotLightComponent
        if (auto slNode = entityNode["SpotLightComponent"]) {
            auto& sl = entity.AddComponent<SpotLightComponent>();
            if (auto d = slNode["Direction"]) sl.Direction = { d[0].as<float>(), d[1].as<float>(), d[2].as<float>() };
            if (auto c = slNode["Color"]) sl.Color = { c[0].as<float>(), c[1].as<float>(), c[2].as<float>() };
            if (slNode["Intensity"]) sl.Intensity = slNode["Intensity"].as<float>();
            if (slNode["Range"]) sl.Range = slNode["Range"].as<float>();
            if (slNode["InnerAngle"]) sl.InnerAngle = slNode["InnerAngle"].as<float>();
            if (slNode["OuterAngle"]) sl.OuterAngle = slNode["OuterAngle"].as<float>();
        }

        // CameraComponent
        if (auto camNode = entityNode["CameraComponent"]) {
            auto& cam = entity.AddComponent<CameraComponent>();
            std::string pt = camNode["ProjectionType"].as<std::string>("Perspective");
            cam.ProjectionType = (pt == "Orthographic") ? CameraProjection::Orthographic : CameraProjection::Perspective;
            if (camNode["FOV"]) cam.FOV = camNode["FOV"].as<float>();
            if (camNode["NearClip"]) cam.NearClip = camNode["NearClip"].as<float>();
            if (camNode["FarClip"]) cam.FarClip = camNode["FarClip"].as<float>();
        }

        // Track root entity (first one without parent)
        if (!rootEntity.IsValid() && !entityNode["Parent"])
            rootEntity = entity;
    }

    // Second pass: resolve parent-child using remap
    for (auto& [newUUID, oldParentUUID] : parentMap) {
        auto remapIt = uuidRemap.find(oldParentUUID);
        if (remapIt == uuidRemap.end()) continue;
        uint64_t newParentUUID = remapIt->second;

        auto childIt = newEntityMap.find(newUUID);
        auto parentIt = newEntityMap.find(newParentUUID);
        if (childIt != newEntityMap.end() && parentIt != newEntityMap.end()) {
            scene.SetParent(childIt->second, parentIt->second);
        }
    }

    VE_ENGINE_INFO("Prefab instantiated: {} ({} entities)", filepath,
                    static_cast<int>(newEntityMap.size()));
    return rootEntity;
}

// ── Clipboard copy-paste ──────────────────────────────────────────

std::string SceneSerializer::SerializeEntityToString(Entity rootEntity, Scene& scene) {
    auto& reg = scene.GetRegistry();
    if (!rootEntity.IsValid()) return {};

    // Collect root + all descendants
    std::vector<entt::entity> entities;
    CollectEntityHierarchy(rootEntity.GetHandle(), reg, entities);

    // Serialize using the same format as prefab
    YAML::Emitter out;
    out << YAML::BeginMap;

    auto& tag = rootEntity.GetComponent<TagComponent>();
    out << YAML::Key << "Prefab" << YAML::Value << tag.Tag;

    out << YAML::Key << "Entities" << YAML::Value << YAML::BeginSeq;
    for (auto e : entities) {
        Entity entity(e, &scene);
        SerializeEntity(out, entity, reg);
    }
    out << YAML::EndSeq;
    out << YAML::EndMap;

    return std::string(out.c_str());
}

Entity SceneSerializer::InstantiateFromString(const std::string& yamlData, Scene& scene) {
    if (yamlData.empty()) return {};

    YAML::Node data;
    try {
        data = YAML::Load(yamlData);
    } catch (...) {
        VE_ENGINE_ERROR("Failed to parse clipboard entity data");
        return {};
    }

    if (!data["Prefab"] || !data["Entities"]) {
        VE_ENGINE_ERROR("Invalid clipboard entity format");
        return {};
    }

    auto& reg = scene.GetRegistry();

    // First pass: create entities with new UUIDs, build remap table
    std::unordered_map<uint64_t, uint64_t> uuidRemap; // old -> new
    std::unordered_map<uint64_t, entt::entity> newEntityMap; // new uuid -> handle
    std::unordered_map<uint64_t, uint64_t> parentMap; // new uuid -> old parent uuid

    Entity rootEntity;

    for (auto entityNode : data["Entities"]) {
        uint64_t oldUUID = entityNode["Entity"].as<uint64_t>();
        UUID newUUID; // generate fresh
        uuidRemap[oldUUID] = static_cast<uint64_t>(newUUID);

        std::string name = "Entity";
        std::string goTag = "Untagged";
        int layer = 0;
        bool active = true;
        if (auto tagNode = entityNode["TagComponent"]) {
            name = tagNode["Tag"].as<std::string>("Entity");
            if (tagNode["GameObjectTag"]) goTag = tagNode["GameObjectTag"].as<std::string>();
            if (tagNode["Layer"]) layer = tagNode["Layer"].as<int>();
            if (tagNode["Active"]) active = tagNode["Active"].as<bool>();
        }

        Entity entity = scene.CreateEntityWithUUID(newUUID, name);
        auto& tc = entity.GetComponent<TagComponent>();
        tc.GameObjectTag = goTag;
        tc.Layer = layer;
        tc.Active = active;

        newEntityMap[static_cast<uint64_t>(newUUID)] = entity.GetHandle();

        // Track parent reference
        if (entityNode["Parent"]) {
            parentMap[static_cast<uint64_t>(newUUID)] = entityNode["Parent"].as<uint64_t>();
        }

        // Deserialize transform
        if (auto tcNode = entityNode["TransformComponent"]) {
            auto& t = entity.GetComponent<TransformComponent>();
            auto pos = tcNode["Position"];
            t.Position = { pos[0].as<float>(), pos[1].as<float>(), pos[2].as<float>() };
            auto rot = tcNode["Rotation"];
            t.Rotation = { rot[0].as<float>(), rot[1].as<float>(), rot[2].as<float>() };
            auto scl = tcNode["Scale"];
            t.Scale = { scl[0].as<float>(), scl[1].as<float>(), scl[2].as<float>() };
        }

        // MeshRendererComponent
        if (auto mrNode = entityNode["MeshRendererComponent"]) {
            auto& mr = entity.AddComponent<MeshRendererComponent>();
            int meshIndex = mrNode["MeshType"].as<int>();
            if (meshIndex >= 0 && meshIndex < MeshLibrary::GetMeshCount()) {
                mr.Mesh = MeshLibrary::GetMeshByIndex(meshIndex);
                mr.Mat = MeshLibrary::IsLitMesh(meshIndex)
                    ? MaterialLibrary::Get("Lit") : MaterialLibrary::Get("Default");
                mr.LocalBounds = MeshLibrary::GetMeshAABB(meshIndex);
            } else if (meshIndex == -1 && mrNode["MeshSource"]) {
                mr.MeshSourcePath = mrNode["MeshSource"].as<std::string>();
                auto meshAsset = MeshImporter::GetOrLoad(mr.MeshSourcePath);
                if (meshAsset && meshAsset->VAO) {
                    mr.Mesh = meshAsset->VAO;
                    mr.Mat = MaterialLibrary::Get("Lit");
                    mr.LocalBounds = meshAsset->BoundingBox;
                }
            }
            if (auto colorNode = mrNode["Color"])
                mr.Color = { colorNode[0].as<float>(), colorNode[1].as<float>(),
                             colorNode[2].as<float>(), colorNode[3].as<float>() };
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
            if (meshIndex >= 0 && MeshLibrary::IsLitMesh(meshIndex) && mr.Mat) {
                if (mr.Mat->GetName() == "Default")
                    mr.Mat = MaterialLibrary::Get("Lit");
            }
            if (auto castNode = mrNode["CastShadows"])
                mr.CastShadows = castNode.as<bool>();
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
                        ov.TexturePath = propNode["Value"].as<std::string>("");
                        if (!ov.TexturePath.empty() &&
                            ov.TexturePath != "white" && ov.TexturePath != "black" &&
                            ov.TexturePath != "bump" && ov.TexturePath != "gray")
                            ov.TextureRef = Texture2D::Create(ov.TexturePath);
                        else
                            ov.TexturePath.clear();
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
        }

        // RigidbodyComponent
        if (auto rbNode = entityNode["RigidbodyComponent"]) {
            auto& rb = entity.AddComponent<RigidbodyComponent>();
            std::string bt = rbNode["BodyType"].as<std::string>("Dynamic");
            rb.Type = (bt == "Static") ? BodyType::Static :
                      (bt == "Kinematic") ? BodyType::Kinematic : BodyType::Dynamic;
            if (rbNode["Mass"]) rb.Mass = rbNode["Mass"].as<float>();
            if (rbNode["LinearDamping"]) rb.LinearDamping = rbNode["LinearDamping"].as<float>();
            if (rbNode["AngularDamping"]) rb.AngularDamping = rbNode["AngularDamping"].as<float>();
            if (rbNode["Restitution"]) rb.Restitution = rbNode["Restitution"].as<float>();
            if (rbNode["Friction"]) rb.Friction = rbNode["Friction"].as<float>();
            if (rbNode["UseGravity"]) rb.UseGravity = rbNode["UseGravity"].as<bool>();
        }

        // Colliders
        if (auto bcNode = entityNode["BoxColliderComponent"]) {
            auto& bc = entity.AddComponent<BoxColliderComponent>();
            if (auto s = bcNode["Size"]) bc.Size = { s[0].as<float>(), s[1].as<float>(), s[2].as<float>() };
            if (auto o = bcNode["Offset"]) bc.Offset = { o[0].as<float>(), o[1].as<float>(), o[2].as<float>() };
        }
        if (auto scNode = entityNode["SphereColliderComponent"]) {
            auto& sc = entity.AddComponent<SphereColliderComponent>();
            if (scNode["Radius"]) sc.Radius = scNode["Radius"].as<float>();
            if (auto o = scNode["Offset"]) sc.Offset = { o[0].as<float>(), o[1].as<float>(), o[2].as<float>() };
        }
        if (auto n = entityNode["CapsuleColliderComponent"]) {
            auto& col = entity.AddComponent<CapsuleColliderComponent>();
            if (n["Radius"]) col.Radius = n["Radius"].as<float>();
            if (n["Height"]) col.Height = n["Height"].as<float>();
            if (auto o = n["Offset"]) col.Offset = { o[0].as<float>(), o[1].as<float>(), o[2].as<float>() };
        }
        if (auto n = entityNode["MeshColliderComponent"]) {
            auto& col = entity.AddComponent<MeshColliderComponent>();
            col.Convex = n["Convex"].as<bool>(true);
            if (auto o = n["Offset"]) col.Offset = { o[0].as<float>(), o[1].as<float>(), o[2].as<float>() };
        }

        // ScriptComponent
        if (auto scrNode = entityNode["ScriptComponent"]) {
            auto& sc = entity.AddComponent<ScriptComponent>();
            sc.ClassName = scrNode["ClassName"].as<std::string>("");
            if (auto propsNode = scrNode["Properties"]) {
                for (auto it = propsNode.begin(); it != propsNode.end(); ++it) {
                    std::string key = it->first.as<std::string>();
                    auto& val = it->second;
                    if (val.Scalar() == "true" || val.Scalar() == "false") {
                        sc.Properties[key] = val.as<bool>();
                    } else {
                        try {
                            float f = val.as<float>();
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

        // DirectionalLightComponent
        if (auto dlNode = entityNode["DirectionalLightComponent"]) {
            auto& dl = entity.AddComponent<DirectionalLightComponent>();
            if (auto d = dlNode["Direction"]) dl.Direction = { d[0].as<float>(), d[1].as<float>(), d[2].as<float>() };
            if (auto c = dlNode["Color"]) dl.Color = { c[0].as<float>(), c[1].as<float>(), c[2].as<float>() };
            if (dlNode["Intensity"]) dl.Intensity = dlNode["Intensity"].as<float>();
        }

        // PointLightComponent
        if (auto plNode = entityNode["PointLightComponent"]) {
            auto& pl = entity.AddComponent<PointLightComponent>();
            if (auto c = plNode["Color"]) pl.Color = { c[0].as<float>(), c[1].as<float>(), c[2].as<float>() };
            if (plNode["Intensity"]) pl.Intensity = plNode["Intensity"].as<float>();
            if (plNode["Range"]) pl.Range = plNode["Range"].as<float>();
        }

        // CameraComponent
        if (auto camNode = entityNode["CameraComponent"]) {
            auto& cam = entity.AddComponent<CameraComponent>();
            std::string pt = camNode["ProjectionType"].as<std::string>("Perspective");
            cam.ProjectionType = (pt == "Orthographic") ? CameraProjection::Orthographic : CameraProjection::Perspective;
            if (camNode["FOV"]) cam.FOV = camNode["FOV"].as<float>();
            if (camNode["Size"]) cam.Size = camNode["Size"].as<float>();
            if (camNode["NearClip"]) cam.NearClip = camNode["NearClip"].as<float>();
            if (camNode["FarClip"]) cam.FarClip = camNode["FarClip"].as<float>();
            if (camNode["Priority"]) cam.Priority = camNode["Priority"].as<int>();
        }

        // AudioSourceComponent
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

        // AudioListenerComponent
        if (entityNode["AudioListenerComponent"]) {
            entity.AddComponent<AudioListenerComponent>();
        }

        // SpriteRendererComponent
        if (auto srNode = entityNode["SpriteRendererComponent"]) {
            auto& sr = entity.AddComponent<SpriteRendererComponent>();
            if (auto c = srNode["Color"])
                sr.Color = { c[0].as<float>(), c[1].as<float>(), c[2].as<float>(), c[3].as<float>() };
            if (srNode["TexturePath"]) {
                sr.TexturePath = srNode["TexturePath"].as<std::string>();
                if (!sr.TexturePath.empty())
                    sr.Texture = Texture2D::Create(sr.TexturePath);
            }
            if (auto uv = srNode["UVRect"])
                sr.UVRect = { uv[0].as<float>(), uv[1].as<float>(), uv[2].as<float>(), uv[3].as<float>() };
            if (srNode["SortingOrder"]) sr.SortingOrder = srNode["SortingOrder"].as<int>();
        }

        // SpriteAnimatorComponent
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

        // ParticleSystemComponent
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

        // AnimatorComponent
        if (auto acNode = entityNode["AnimatorComponent"]) {
            auto& ac = entity.AddComponent<AnimatorComponent>();
            if (acNode["AnimationSource"]) ac.AnimationSourcePath = acNode["AnimationSource"].as<std::string>();
            if (acNode["ClipIndex"])   ac.ClipIndex   = acNode["ClipIndex"].as<int>();
            if (acNode["PlayOnStart"]) ac.PlayOnStart = acNode["PlayOnStart"].as<bool>();
            if (acNode["Loop"])        ac.Loop        = acNode["Loop"].as<bool>();
            if (acNode["Speed"])       ac.Speed       = acNode["Speed"].as<float>();
        }

        // LODGroupComponent
        if (auto lodNode = entityNode["LODGroupComponent"]) {
            auto& lod = entity.AddComponent<LODGroupComponent>();
            if (lodNode["CullDistance"])
                lod.CullDistance = lodNode["CullDistance"].as<float>();
            if (auto levelsNode = lodNode["Levels"]) {
                for (auto levelNode : levelsNode) {
                    LODLevel level;
                    level.MeshType = levelNode["MeshType"].as<int>(-1);
                    if (level.MeshType >= 0 && level.MeshType < MeshLibrary::GetMeshCount()) {
                        level.Mesh = MeshLibrary::GetMeshByIndex(level.MeshType);
                    } else if (levelNode["MeshSource"]) {
                        level.MeshSourcePath = levelNode["MeshSource"].as<std::string>();
                        auto meshAsset = MeshImporter::GetOrLoad(level.MeshSourcePath);
                        if (meshAsset && meshAsset->VAO)
                            level.Mesh = meshAsset->VAO;
                    }
                    level.MaxDistance = levelNode["MaxDistance"].as<float>(50.0f);
                    lod.Levels.push_back(level);
                }
            }
        }

        // NavAgentComponent
        if (auto navNode = entityNode["NavAgentComponent"]) {
            auto& nav = entity.AddComponent<NavAgentComponent>();
            if (navNode["Speed"])        nav.Speed        = navNode["Speed"].as<float>();
            if (navNode["StoppingDist"]) nav.StoppingDist = navNode["StoppingDist"].as<float>();
            if (navNode["AgentRadius"])  nav.AgentRadius  = navNode["AgentRadius"].as<float>();
        }

        // Track root entity (first one without parent)
        if (!rootEntity.IsValid() && !entityNode["Parent"])
            rootEntity = entity;
    }

    // Second pass: resolve parent-child using remap
    for (auto& [newUUID, oldParentUUID] : parentMap) {
        auto remapIt = uuidRemap.find(oldParentUUID);
        if (remapIt == uuidRemap.end()) continue;
        uint64_t newParentUUID = remapIt->second;

        auto childIt = newEntityMap.find(newUUID);
        auto parentIt = newEntityMap.find(newParentUUID);
        if (childIt != newEntityMap.end() && parentIt != newEntityMap.end()) {
            scene.SetParent(childIt->second, parentIt->second);
        }
    }

    VE_ENGINE_INFO("Entity pasted from clipboard ({} entities)",
                    static_cast<int>(newEntityMap.size()));
    return rootEntity;
}

} // namespace VE
