/*
 * AnimationClip — Sampled keyframe animation data for skeletal animation.
 *
 * Each BoneTrack stores time-keyed position/rotation/scale for one bone.
 * AnimationClip owns all tracks and provides lookup by bone index.
 */
#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>

namespace VE {

struct BoneKeyframe {
    float Time = 0.0f;
    glm::vec3 Position = glm::vec3(0.0f);
    glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // w,x,y,z
    glm::vec3 Scale = glm::vec3(1.0f);
};

struct BoneTrack {
    int BoneIndex = -1;
    std::vector<BoneKeyframe> Keyframes;
};

class AnimationClip {
public:
    std::string Name;
    float Duration = 0.0f;
    std::vector<BoneTrack> Tracks;

    const BoneTrack* FindTrack(int boneIndex) const;
};

} // namespace VE
