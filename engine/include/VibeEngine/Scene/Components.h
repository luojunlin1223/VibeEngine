/*
 * Components — Core ECS component types for VibeEngine.
 *
 * Every entity gets an IDComponent (UUID) and TagComponent (name) by default.
 * TransformComponent holds position/rotation/scale.
 * MeshRendererComponent references a mesh and shader for rendering.
 */
#pragma once

#include "VibeEngine/Core/UUID.h"
#include "VibeEngine/Renderer/VertexArray.h"
#include "VibeEngine/Renderer/Shader.h"
#include "VibeEngine/Renderer/Texture.h"
#include "VibeEngine/Renderer/Material.h"

#include <entt/entt.hpp>
#include <string>
#include <array>
#include <vector>
#include <memory>
#include <unordered_map>
#include <variant>

namespace VE { class Animator; }

namespace VE {

struct IDComponent {
    UUID ID;

    IDComponent() = default;
    IDComponent(UUID id) : ID(id) {}
};

// Built-in tags (user can extend at runtime)
inline const char* kDefaultTags[] = {
    "Untagged", "Respawn", "Finish", "EditorOnly",
    "MainCamera", "Player", "GameController"
};
inline constexpr int kDefaultTagCount = 7;

// Built-in layer names (32 layers, 0-31)
inline const char* kDefaultLayers[] = {
    "Default",        // 0
    "TransparentFX",  // 1
    "Ignore Raycast", // 2
    "",               // 3
    "Water",          // 4
    "UI",             // 5
    "", "", "",       // 6-8
    "", "", "", "", "", "", "", // 9-15
    "", "", "", "", "", "", "", "", // 16-23
    "", "", "", "", "", "", "", ""  // 24-31
};
inline constexpr int kLayerCount = 32;

struct TagComponent {
    std::string Tag;                         // entity name (like Unity's gameObject.name)
    std::string GameObjectTag = "Untagged";  // categorical tag (like Unity's gameObject.tag)
    int Layer = 0;                           // layer index 0-31 (like Unity's gameObject.layer)

    TagComponent() = default;
    TagComponent(const std::string& tag) : Tag(tag) {}
};

struct RelationshipComponent {
    entt::entity Parent = entt::null;
    std::vector<entt::entity> Children;

    RelationshipComponent() = default;
};

struct TransformComponent {
    std::array<float, 3> Position = { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> Rotation = { 0.0f, 0.0f, 0.0f }; // degrees
    std::array<float, 3> Scale    = { 1.0f, 1.0f, 1.0f };

    TransformComponent() = default;
};

struct DirectionalLightComponent {
    std::array<float, 3> Direction = { 0.3f, 1.0f, 0.5f }; // world-space direction (auto-normalized)
    std::array<float, 3> Color    = { 1.0f, 1.0f, 1.0f };
    float Intensity = 1.0f;

    DirectionalLightComponent() = default;
};

struct PointLightComponent {
    std::array<float, 3> Color = { 1.0f, 1.0f, 1.0f };
    float Intensity = 1.0f;
    float Range     = 10.0f; // world-space radius of influence

    PointLightComponent() = default;
};

enum class BodyType { Static, Kinematic, Dynamic };

struct RigidbodyComponent {
    BodyType Type = BodyType::Dynamic;
    float Mass = 1.0f;
    float LinearDamping = 0.05f;
    float AngularDamping = 0.05f;
    float Restitution = 0.3f;
    float Friction = 0.5f;
    bool UseGravity = true;
    uint32_t _JoltBodyID = 0xFFFFFFFF; // runtime only, not serialized

    RigidbodyComponent() = default;
};

// Individual collider components (Unity-style: one component per collider type)

struct BoxColliderComponent {
    std::array<float, 3> Size   = { 1.0f, 1.0f, 1.0f }; // full extents (width, height, depth)
    std::array<float, 3> Offset = { 0.0f, 0.0f, 0.0f };
    BoxColliderComponent() = default;
};

struct SphereColliderComponent {
    float Radius = 0.5f;
    std::array<float, 3> Offset = { 0.0f, 0.0f, 0.0f };
    SphereColliderComponent() = default;
};

struct CapsuleColliderComponent {
    float Radius = 0.5f;
    float Height = 2.0f; // total height including hemispheres
    std::array<float, 3> Offset = { 0.0f, 0.0f, 0.0f };
    CapsuleColliderComponent() = default;
};

struct MeshColliderComponent {
    bool Convex = true; // true = ConvexHullShape (dynamic OK), false = MeshShape (static only)
    std::array<float, 3> Offset = { 0.0f, 0.0f, 0.0f };
    // Mesh data sourced from entity's MeshRendererComponent at physics start
    MeshColliderComponent() = default;
};

// Forward declaration — full definition in Scripting/NativeScript.h
class NativeScript;

// Stored property value — persists across play/stop, serialized in scene files
using ScriptPropertyValue = std::variant<float, int, bool>;

struct ScriptComponent {
    std::string ClassName;               // e.g. "PlayerController"
    NativeScript* _Instance = nullptr;   // runtime only, not serialized

    // Property values — editable in Inspector, applied to instance on play
    std::unordered_map<std::string, ScriptPropertyValue> Properties;

    ScriptComponent() = default;
    ScriptComponent(const std::string& cls) : ClassName(cls) {}
};

struct AnimatorComponent {
    std::string AnimationSourcePath; // FBX file with animation clips (can differ from mesh FBX)
    int   ClipIndex = 0;
    bool  PlayOnStart = true;
    bool  Loop = true;
    float Speed = 1.0f;
    std::shared_ptr<Animator> _Animator; // runtime only, not serialized

    AnimatorComponent() = default;
};

enum class CameraProjection { Perspective, Orthographic };

struct CameraComponent {
    CameraProjection ProjectionType = CameraProjection::Perspective;
    float FOV  = 60.0f;          // degrees (perspective)
    float Size = 5.0f;           // half-height in world units (orthographic)
    float NearClip = 0.1f;
    float FarClip  = 1000.0f;
    int   Priority = 0;          // highest priority when multiple MainCamera tags exist

    CameraComponent() = default;
};

struct AudioSourceComponent {
    std::string ClipPath;           // path to audio file (.wav, .mp3, .ogg)
    float Volume = 1.0f;           // 0.0 – 1.0
    float Pitch  = 1.0f;           // 0.1 – 3.0
    bool  Loop   = false;
    bool  Spatial = false;          // true = 3D positional audio
    bool  PlayOnAwake = true;
    float MinDistance = 1.0f;       // 3D: distance at full volume
    float MaxDistance = 100.0f;     // 3D: distance at zero volume

    // Runtime only (not serialized)
    uint32_t _SoundHandle = 0;

    AudioSourceComponent() = default;
};

struct AudioListenerComponent {
    // Marker component — the entity with this + TransformComponent is the listener.
    bool Active = true;
    AudioListenerComponent() = default;
};

struct SpriteRendererComponent {
    std::array<float, 4> Color = { 1.0f, 1.0f, 1.0f, 1.0f }; // RGBA tint
    std::shared_ptr<Texture2D> Texture;
    std::string TexturePath;
    // UV rect for sprite atlas (x, y, width, height in 0..1)
    std::array<float, 4> UVRect = { 0.0f, 0.0f, 1.0f, 1.0f };
    int SortingOrder = 0;

    SpriteRendererComponent() = default;
};

struct SpriteAnimatorComponent {
    int Columns = 1;
    int Rows    = 1;
    int StartFrame = 0;
    int EndFrame   = 0;
    float FrameRate = 12.0f;
    bool Loop = true;
    bool PlayOnStart = true;

    // Runtime (not serialized)
    bool  _Playing = false;
    float _Timer = 0.0f;
    int   _CurrentFrame = 0;

    SpriteAnimatorComponent() = default;
};

struct MeshRendererComponent {
    std::shared_ptr<VertexArray>  Mesh;
    std::shared_ptr<VE::Material> Mat;            // material (shader + properties)
    std::array<float, 4>          Color = { 1.0f, 1.0f, 1.0f, 1.0f }; // per-instance color override
    std::string                   MaterialPath;   // .vmat path for custom materials
    std::string                   MeshSourcePath; // for imported meshes (FBX etc.)
    bool                          CastShadows = true;

    /// Per-entity material property overrides (like script property reflection).
    /// Populated from material defaults, editable per-entity, serialized with scene.
    std::vector<VE::MaterialProperty> MaterialOverrides;

    MeshRendererComponent() = default;
};

} // namespace VE
