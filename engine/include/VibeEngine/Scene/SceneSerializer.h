/*
 * SceneSerializer — Saves and loads scenes in YAML format (.vscene).
 *
 * Inspired by Unity's scene file approach: each entity is serialized
 * with a stable UUID, and components are written as key-value pairs.
 * Mesh references use MeshLibrary indices for portability.
 */
#pragma once

#include "Scene.h"
#include <string>
#include <memory>

namespace VE {

class SceneSerializer {
public:
    SceneSerializer(const std::shared_ptr<Scene>& scene);

    void Serialize(const std::string& filepath);
    bool Deserialize(const std::string& filepath);

private:
    std::shared_ptr<Scene> m_Scene;
};

} // namespace VE
