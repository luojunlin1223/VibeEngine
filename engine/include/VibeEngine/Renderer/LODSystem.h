/*
 * LODSystem — Level of Detail mesh selection based on camera distance.
 *
 * LODGroupComponent stores a chain of LOD levels (mesh + transition distance).
 * During rendering, the appropriate LOD mesh is selected per entity.
 * Supports both built-in meshes and imported MeshAssets.
 */
#pragma once

#include "VibeEngine/Renderer/VertexArray.h"
#include "VibeEngine/Asset/MeshAsset.h"
#include <memory>
#include <vector>
#include <string>

namespace VE {

// A single LOD level: a mesh and the max distance at which it's used
struct LODLevel {
    std::shared_ptr<VertexArray> Mesh;
    std::string MeshSourcePath;  // for imported meshes (serialization)
    int MeshType = -1;           // built-in mesh index, or -1 for imported
    float ScreenRelativeHeight = 0.0f; // Unity-style (0-1), not used yet
    float MaxDistance = 0.0f;    // beyond this distance, switch to next LOD
};

// Stored as an ECS component on entities that use LOD
struct LODGroupComponent {
    std::vector<LODLevel> Levels;  // sorted by distance (closest first)
    float CullDistance = 0.0f;     // distance beyond last LOD to cull entirely (0 = never cull)
    bool FadeTransition = false;   // future: cross-fade between LODs

    // Currently active LOD index (runtime, not serialized)
    int _ActiveLOD = 0;

    LODGroupComponent() = default;
};

// Utility: select LOD index based on distance
inline int SelectLOD(const LODGroupComponent& lodGroup, float distance) {
    for (int i = 0; i < static_cast<int>(lodGroup.Levels.size()); ++i) {
        if (distance <= lodGroup.Levels[i].MaxDistance)
            return i;
    }
    // Beyond all LOD distances
    if (lodGroup.CullDistance > 0.0f && distance > lodGroup.CullDistance)
        return -1; // cull
    // Use last LOD
    return lodGroup.Levels.empty() ? 0 : static_cast<int>(lodGroup.Levels.size()) - 1;
}

// Utility: generate LOD spheres with varying detail
struct LODMeshGenerator {
    // Create a UV sphere with given subdivision. Vertex layout matches lit shader:
    // pos(3) + normal(3) + color(3) + uv(2) = 11 floats per vertex
    static std::shared_ptr<MeshAsset> CreateSphere(int rings, int segments);

    // Create a cube with given subdivision per face
    static std::shared_ptr<MeshAsset> CreateCube();
};

} // namespace VE
