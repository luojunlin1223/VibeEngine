#include "VibeEngine/Animation/Skeleton.h"

namespace VE {

int Skeleton::FindBoneIndex(const std::string& name) const {
    for (int i = 0; i < static_cast<int>(Bones.size()); i++) {
        if (Bones[i].Name == name)
            return i;
    }
    return -1;
}

} // namespace VE
