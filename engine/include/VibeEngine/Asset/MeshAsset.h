#pragma once

#include "VibeEngine/Core/Object.h"
#include "VibeEngine/Renderer/VertexArray.h"
#include "VibeEngine/Animation/AnimationClip.h"
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <memory>
#include <limits>

namespace VE {

class Skeleton;

// Per-vertex skin weights (up to 4 bone influences)
struct SkinVertex {
    int   BoneIndices[4] = { 0, 0, 0, 0 };
    float BoneWeights[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
};

struct AABB {
    glm::vec3 Min = glm::vec3( std::numeric_limits<float>::max());
    glm::vec3 Max = glm::vec3(-std::numeric_limits<float>::max());

    glm::vec3 Center() const { return (Min + Max) * 0.5f; }
    glm::vec3 Extents() const { return (Max - Min) * 0.5f; }
    bool Valid() const { return Min.x <= Max.x; }
};

struct MeshAsset : public Object {
    std::string SourcePath; // absolute path to source file

    // CPU data: interleaved pos(3)+normal(3)+color(3)+uv(2) = 11 floats per vertex
    std::vector<float>    Vertices;
    std::vector<uint32_t> Indices;

    // GPU data
    std::shared_ptr<VertexArray> VAO;

    // Local-space bounding box (computed from vertex positions)
    AABB BoundingBox;

    // Skinning data (populated by FBXImporter for skinned meshes)
    std::vector<float>      BindPoseVertices; // immutable copy of original vertices
    std::vector<SkinVertex> SkinData;         // per-vertex bone indices + weights
    std::shared_ptr<Skeleton> SkeletonRef;
    std::vector<AnimationClip> Clips;

    bool IsSkinned() const { return SkeletonRef != nullptr && !SkinData.empty(); }

    void Upload();  // create GPU buffers from CPU data
    void Release(); // destroy GPU buffers (keeps CPU data)
    void ComputeBoundingBox(); // compute AABB from vertex positions
};

} // namespace VE
