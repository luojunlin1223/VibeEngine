/*
 * Skeleton — Bone hierarchy extracted from skinned meshes.
 *
 * Each Bone stores its name, parent index (-1 for root), inverse bind matrix,
 * and local bind transform. The Skeleton owns the flat bone array and provides
 * lookup by name.
 */
#pragma once

#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace VE {

struct Bone {
    std::string Name;
    int ParentIndex = -1; // -1 = root
    glm::mat4 InverseBindMatrix = glm::mat4(1.0f);
    glm::mat4 LocalBindTransform = glm::mat4(1.0f);
};

class Skeleton {
public:
    std::vector<Bone> Bones;
    float ImportScale = 1.0f; // scale factor applied during FBX import

    int FindBoneIndex(const std::string& name) const;
    int GetBoneCount() const { return static_cast<int>(Bones.size()); }
};

} // namespace VE
