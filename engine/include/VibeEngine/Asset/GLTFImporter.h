/*
 * GLTFImporter.h — glTF/GLB model import using cgltf
 *
 * Design: Parses glTF 2.0 and GLB files, extracts mesh geometry
 * (positions, normals, UVs, indices) and converts to the engine's
 * interleaved vertex layout: pos(3)+normal(3)+color(3)+uv(2) = 11 floats.
 * Supports merging all primitives/meshes into a single MeshAsset.
 */
#pragma once

#include "VibeEngine/Asset/MeshAsset.h"
#include <string>
#include <memory>

namespace VE {

class GLTFImporter {
public:
    // Import mesh from a .gltf or .glb file.
    // Merges all meshes/primitives into a single MeshAsset.
    // Returns nullptr on failure.
    static std::shared_ptr<MeshAsset> Import(const std::string& absPath);
};

} // namespace VE
