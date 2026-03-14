/*
 * Frustum — View frustum extracted from a VP matrix for culling.
 *
 * Extracts six clip planes (left, right, bottom, top, near, far)
 * and tests axis-aligned bounding boxes against them.
 */
#pragma once

#include <glm/glm.hpp>
#include "VibeEngine/Asset/MeshAsset.h" // for AABB

namespace VE {

struct Plane {
    glm::vec3 Normal;
    float     Distance;

    void Normalize() {
        float len = glm::length(Normal);
        if (len > 0.0f) {
            Normal   /= len;
            Distance /= len;
        }
    }

    float DistanceTo(const glm::vec3& point) const {
        return glm::dot(Normal, point) + Distance;
    }
};

class Frustum {
public:
    Frustum() = default;

    // Extract frustum planes from a combined View-Projection matrix
    // Uses the Griess-Hartmann method
    explicit Frustum(const glm::mat4& vp) { Extract(vp); }

    void Extract(const glm::mat4& vp) {
        // Left
        m_Planes[0].Normal   = { vp[0][3] + vp[0][0], vp[1][3] + vp[1][0], vp[2][3] + vp[2][0] };
        m_Planes[0].Distance =  vp[3][3] + vp[3][0];
        // Right
        m_Planes[1].Normal   = { vp[0][3] - vp[0][0], vp[1][3] - vp[1][0], vp[2][3] - vp[2][0] };
        m_Planes[1].Distance =  vp[3][3] - vp[3][0];
        // Bottom
        m_Planes[2].Normal   = { vp[0][3] + vp[0][1], vp[1][3] + vp[1][1], vp[2][3] + vp[2][1] };
        m_Planes[2].Distance =  vp[3][3] + vp[3][1];
        // Top
        m_Planes[3].Normal   = { vp[0][3] - vp[0][1], vp[1][3] - vp[1][1], vp[2][3] - vp[2][1] };
        m_Planes[3].Distance =  vp[3][3] - vp[3][1];
        // Near
        m_Planes[4].Normal   = { vp[0][3] + vp[0][2], vp[1][3] + vp[1][2], vp[2][3] + vp[2][2] };
        m_Planes[4].Distance =  vp[3][3] + vp[3][2];
        // Far
        m_Planes[5].Normal   = { vp[0][3] - vp[0][2], vp[1][3] - vp[1][2], vp[2][3] - vp[2][2] };
        m_Planes[5].Distance =  vp[3][3] - vp[3][2];

        for (auto& p : m_Planes)
            p.Normalize();
    }

    // Test an AABB (world-space) against the frustum.
    // Returns true if the AABB is at least partially inside.
    bool TestAABB(const glm::vec3& min, const glm::vec3& max) const {
        for (const auto& plane : m_Planes) {
            // Find the positive vertex (P-vertex): the corner most aligned with the normal
            glm::vec3 pVertex;
            pVertex.x = (plane.Normal.x >= 0.0f) ? max.x : min.x;
            pVertex.y = (plane.Normal.y >= 0.0f) ? max.y : min.y;
            pVertex.z = (plane.Normal.z >= 0.0f) ? max.z : min.z;

            if (plane.DistanceTo(pVertex) < 0.0f)
                return false; // entirely outside this plane
        }
        return true;
    }

    // Convenience: test an AABB struct
    bool TestAABB(const AABB& aabb) const {
        return TestAABB(aabb.Min, aabb.Max);
    }

    // Test a bounding sphere
    bool TestSphere(const glm::vec3& center, float radius) const {
        for (const auto& plane : m_Planes) {
            if (plane.DistanceTo(center) < -radius)
                return false;
        }
        return true;
    }

private:
    Plane m_Planes[6];
};

} // namespace VE
