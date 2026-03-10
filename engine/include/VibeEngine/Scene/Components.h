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

#include <string>
#include <array>
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

struct TransformComponent {
    std::array<float, 3> Position = { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> Rotation = { 0.0f, 0.0f, 0.0f }; // degrees
    std::array<float, 3> Scale    = { 1.0f, 1.0f, 1.0f };

    TransformComponent() = default;
};

struct MeshRendererComponent {
    std::shared_ptr<VertexArray> Mesh;
    std::shared_ptr<Shader>     Material;
    std::array<float, 4>        Color = { 1.0f, 1.0f, 1.0f, 1.0f };
    std::shared_ptr<Texture2D>  Texture;      // optional diffuse texture
    std::string                 TexturePath;  // path for serialization/reload

    MeshRendererComponent() = default;
};

} // namespace VE
