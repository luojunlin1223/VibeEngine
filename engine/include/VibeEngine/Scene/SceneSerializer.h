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

    // In-memory serialization for play-mode snapshots
    std::string SerializeToString();
    bool DeserializeFromString(const std::string& yamlData);

    // ── Prefab system ───────────────────────────────────────────────
    // Save entity (+ children) as .vprefab
    static void SerializePrefab(const std::string& filepath, Entity rootEntity, Scene& scene);
    // Instantiate a .vprefab into scene, returns root entity
    static Entity InstantiatePrefab(const std::string& filepath, Scene& scene);

    // ── Clipboard / copy-paste ──────────────────────────────────────
    // Serialize entity (+ children) to YAML string for clipboard
    static std::string SerializeEntityToString(Entity rootEntity, Scene& scene);
    // Instantiate entity (+ children) from clipboard YAML string, returns root entity
    static Entity InstantiateFromString(const std::string& yamlData, Scene& scene);

private:
    std::shared_ptr<Scene> m_Scene;
};

} // namespace VE
