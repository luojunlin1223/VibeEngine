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
