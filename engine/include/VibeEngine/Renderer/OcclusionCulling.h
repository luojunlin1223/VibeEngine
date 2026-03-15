/*
 * OcclusionCulling -- GPU-based occlusion culling using OpenGL hardware queries.
 *
 * Uses GL_ANY_SAMPLES_PASSED occlusion queries to test entity bounding boxes
 * against the depth buffer. Results from the previous frame are used to avoid
 * GPU pipeline stalls (temporal occlusion). Entities whose bounding boxes had
 * zero visible samples last frame are considered occluded and skipped.
 *
 * Usage flow per frame:
 *   1. BeginFrame()                  -- swap query buffers, collect last frame's results
 *   2. For each frustum-visible entity:
 *        if (IsOccluded(entityID))   -- check previous frame result
 *            skip rendering
 *        else
 *            BeginQuery(entityID)    -- start occlusion query for this entity
 *            ... render bounding box (depth-only) ...
 *            EndQuery(entityID)
 *   3. EndFrame()                    -- finalize
 */
#pragma once

#include "VibeEngine/Asset/MeshAsset.h" // for AABB
#include <glm/glm.hpp>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <memory>

namespace VE {

class Shader;
class VertexArray;

class OcclusionCulling {
public:
    static void Init();
    static void Shutdown();

    /// Call once at the start of each frame before any occlusion tests.
    /// Reads back previous-frame query results and prepares for new queries.
    static void BeginFrame();

    /// Call once at the end of the frame after all queries have been issued.
    static void EndFrame();

    /// Check if an entity was occluded last frame (returns true => skip rendering).
    /// Entities not yet tracked are assumed visible on their first frame.
    static bool IsOccluded(uint32_t entityID);

    /// Issue an occlusion query for the given entity's world-space AABB.
    /// Renders a depth-only bounding box. Call between BeginFrame/EndFrame.
    static void QueryEntity(uint32_t entityID,
                            const glm::vec3& aabbMin,
                            const glm::vec3& aabbMax,
                            const glm::mat4& viewProjection);

    /// Remove tracking data for a destroyed entity.
    static void RemoveEntity(uint32_t entityID);

    /// Clear all tracked entities and queries (e.g., on scene change).
    static void Reset();

    /// Statistics for the current frame.
    static uint32_t GetOccludedCount();

private:
    static void CreateBoxVAO();
    static void CreateDepthOnlyShader();
};

} // namespace VE
