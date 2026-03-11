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

namespace VE {

struct IDComponent {
    UUID ID;

    IDComponent() = default;
    IDComponent(UUID id) : ID(id) {}
};

struct TagComponent {
    std::string Tag;

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

enum class ColliderShape { Box, Sphere, Capsule };

struct ColliderComponent {
    ColliderShape Shape = ColliderShape::Box;
    std::array<float, 3> Size   = { 1.0f, 1.0f, 1.0f }; // box half-extents, or radius in [0]
    std::array<float, 3> Offset = { 0.0f, 0.0f, 0.0f };

    ColliderComponent() = default;
};

struct MeshRendererComponent {
    std::shared_ptr<VertexArray>  Mesh;
    std::shared_ptr<VE::Material> Mat;            // material (shader + properties)
    std::array<float, 4>          Color = { 1.0f, 1.0f, 1.0f, 1.0f }; // per-instance color override
    std::string                   MaterialPath;   // .vmat path for custom materials
    std::string                   MeshSourcePath; // for imported meshes (FBX etc.)

    MeshRendererComponent() = default;
};

} // namespace VE
