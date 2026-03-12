#include "VibeEngine/Animation/AnimationClip.h"

namespace VE {

const BoneTrack* AnimationClip::FindTrack(int boneIndex) const {
    for (auto& track : Tracks) {
        if (track.BoneIndex == boneIndex)
            return &track;
    }
    return nullptr;
}

} // namespace VE
