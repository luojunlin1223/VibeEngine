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
#include "VibeEngine/Asset/MeshAsset.h"
#include "VibeEngine/UI/FontAtlas.h"
#include "VibeEngine/Terrain/Terrain.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <string>
#include <array>
#include <vector>
#include <memory>
#include <unordered_map>
#include <variant>

namespace VE { class Animator; }
#include "VibeEngine/Animation/AnimStateMachine.h"

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
    bool Active = true;                      // like Unity's gameObject.activeSelf

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

struct SpotLightComponent {
    std::array<float, 3> Direction = { 0.0f, -1.0f, 0.0f }; // local-space direction (auto-normalized)
    std::array<float, 3> Color     = { 1.0f, 1.0f, 1.0f };
    float Intensity  = 1.0f;
    float Range      = 15.0f;  // world-space radius of influence
    float InnerAngle = 25.0f;  // degrees — full-brightness cone
    float OuterAngle = 35.0f;  // degrees — falloff cone edge

    SpotLightComponent() = default;
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

    // State machine config (serialized)
    bool UseStateMachine = false;
    std::vector<AnimState>      States;
    std::vector<AnimTransition> Transitions;
    std::vector<AnimParameter>  Parameters;
    int DefaultState = 0;

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

struct Particle {
    glm::vec3 Position{0.0f}, Velocity{0.0f};
    float Lifetime = 0.0f, MaxLife = 1.0f, Size = 0.2f;
    glm::vec4 Color{1.0f};
    bool Active = false;
};

struct ParticleSystemComponent {
    // Serialized config
    float EmissionRate = 10.0f;          // particles/sec
    float ParticleLifetime = 2.0f;       // seconds
    float LifetimeVariance = 0.5f;
    int   MaxParticles = 500;
    std::array<float,3> VelocityMin = {-1.0f, 1.0f, -1.0f};
    std::array<float,3> VelocityMax = { 1.0f, 3.0f,  1.0f};
    std::array<float,3> Gravity = {0.0f, -9.81f, 0.0f};
    std::array<float,4> StartColor = {1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float,4> EndColor   = {1.0f, 1.0f, 1.0f, 0.0f};
    float StartSize = 0.2f, EndSize = 0.0f;
    std::string TexturePath;
    std::shared_ptr<Texture2D> Texture;
    bool PlayOnStart = true;
    // Runtime only (not serialized)
    std::vector<Particle> _Particles;
    float _EmissionAccumulator = 0.0f;
    bool _Playing = false;

    ParticleSystemComponent() = default;
};

struct MeshRendererComponent {
    std::shared_ptr<VertexArray>  Mesh;
    std::shared_ptr<VE::Material> Mat;            // material (shader + properties)
    std::array<float, 4>          Color = { 1.0f, 1.0f, 1.0f, 1.0f }; // per-instance color override
    std::string                   MaterialPath;   // .vmat path for custom materials
    std::string                   MeshSourcePath; // for imported meshes (FBX etc.)
    bool                          CastShadows = true;

    // Local-space AABB for frustum culling (set when mesh is assigned).
    // If invalid (default), a unit AABB [-0.5, 0.5] is assumed.
    AABB LocalBounds;

    /// Per-entity material property overrides (like script property reflection).
    /// Populated from material defaults, editable per-entity, serialized with scene.
    std::vector<VE::MaterialProperty> MaterialOverrides;

    MeshRendererComponent() = default;
};

// ── NavAgent Component ───────────────────────────────────────────────

struct NavAgentComponent {
    float Speed        = 3.5f;
    float StoppingDist = 0.2f;
    float AgentRadius  = 0.4f;

    // Runtime only (not serialized)
    std::vector<std::array<float, 2>> _Path; // XZ waypoints
    int   _PathIndex = 0;
    bool  _HasTarget = false;
    float _TargetX = 0, _TargetZ = 0;

    NavAgentComponent() = default;
};

// ── Terrain Component ─────────────────────────────────────────────────

struct TerrainComponent {
    // Generation params
    int   Resolution = 128;
    float WorldSizeX = 100.0f;
    float WorldSizeZ = 100.0f;
    float HeightScale = 10.0f;
    std::string HeightmapPath; // empty = procedural

    // Procedural noise params
    int   Octaves = 4;
    float Persistence = 0.5f;
    float Lacunarity = 2.0f;
    float NoiseScale = 50.0f;
    int   Seed = 42;

    // Texture layers (4 layers, height-based blending)
    std::array<std::string, 4> LayerTexturePaths;
    std::array<float, 4> LayerTiling = { 0.1f, 0.1f, 0.1f, 0.1f };
    std::array<float, 3> BlendHeights = { 0.25f, 0.5f, 0.75f }; // transitions between layers
    float Roughness = 0.85f;

    // Runtime only (not serialized)
    std::shared_ptr<Terrain> _Terrain;
    std::shared_ptr<VertexArray> _Mesh;
    std::array<std::shared_ptr<Texture2D>, 4> _LayerTextures;
    bool _NeedsRebuild = true;

    TerrainComponent() = default;
};

// ── Runtime UI Components ─────────────────────────────────────────────

enum class UIAnchorType {
    TopLeft, TopCenter, TopRight,
    MiddleLeft, Center, MiddleRight,
    BottomLeft, BottomCenter, BottomRight
};

struct UICanvasComponent {
    bool ScreenSpace = true; // true = screen overlay, false = world-space (future)
    int  SortOrder = 0;      // higher draws on top

    UICanvasComponent() = default;
};

struct UIRectTransformComponent {
    UIAnchorType Anchor = UIAnchorType::TopLeft;
    std::array<float, 2> AnchoredPosition = { 0.0f, 0.0f }; // offset from anchor in pixels
    std::array<float, 2> Size = { 100.0f, 30.0f };           // width, height in pixels
    std::array<float, 2> Pivot = { 0.0f, 0.0f };             // 0,0 = top-left; 0.5,0.5 = center

    UIRectTransformComponent() = default;
};

struct UITextComponent {
    std::string Text = "Text";
    float FontSize = 24.0f;
    std::array<float, 4> Color = { 1.0f, 1.0f, 1.0f, 1.0f };
    std::string FontPath; // empty = default built-in font

    // Runtime only
    std::shared_ptr<FontAtlas> _Font;

    UITextComponent() = default;
};

struct UIImageComponent {
    std::array<float, 4> Color = { 1.0f, 1.0f, 1.0f, 1.0f };
    std::string TexturePath;

    // Runtime only
    std::shared_ptr<Texture2D> _Texture;

    UIImageComponent() = default;
};

struct UIButtonComponent {
    std::string Label = "Button";
    float FontSize = 20.0f;
    std::array<float, 4> LabelColor   = { 1.0f, 1.0f, 1.0f, 1.0f };
    std::array<float, 4> NormalColor  = { 0.2f, 0.2f, 0.2f, 0.8f };
    std::array<float, 4> HoverColor   = { 0.3f, 0.3f, 0.3f, 0.9f };
    std::array<float, 4> PressedColor = { 0.1f, 0.1f, 0.1f, 1.0f };

    // Runtime state (not serialized)
    bool _Hovered = false;
    bool _Pressed = false;
    bool _Clicked = false; // true for one frame on click

    UIButtonComponent() = default;
};

} // namespace VE
