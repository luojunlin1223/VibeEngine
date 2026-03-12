/*
 * FBXImporter.h — Unity-style FBX import with configurable settings
 *
 * Design: FBXImportSettings are stored in the .meta YAML file alongside
 * the uuid/type fields.  FBXImporter reads/writes settings and performs
 * the actual mesh import, delegated to by MeshImporter.
 */
#pragma once

#include "VibeEngine/Asset/MeshAsset.h"
#include <string>
#include <memory>
#include <cstdint>

namespace VE {

struct FBXImportSettings {
    float ScaleFactor = 1.0f;

    enum class NormalMode { Import, Calculate, None };
    NormalMode Normals = NormalMode::Import;

    bool ImportUVs = true;
    bool MergeAllMeshes = true;
    bool ImportVertexColors = false;
    bool ImportSkinWeights = true;
    bool ImportAnimations = true;

    // Read-only mesh info (populated after import)
    uint32_t VertexCount = 0;
    uint32_t TriangleCount = 0;
    uint32_t SubMeshCount = 0;
    uint32_t BoneCount = 0;
    uint32_t ClipCount = 0;
};

class Skeleton;
class AnimationClip;

class FBXImporter {
public:
    // Load/save import settings from/to .meta YAML
    static FBXImportSettings LoadSettings(const std::string& metaPath);
    static void SaveSettings(const std::string& metaPath, const FBXImportSettings& settings);

    // Import mesh with given settings.  Populates info fields on settings.
    static std::shared_ptr<MeshAsset> Import(const std::string& absPath,
                                              FBXImportSettings& settings);

    // Import only animation clips from an FBX, remapping bone names to an existing skeleton.
    // Returns empty vector if no animations found.
    static std::vector<AnimationClip> ImportAnimations(const std::string& absPath,
                                                        const std::shared_ptr<Skeleton>& skeleton);
};

} // namespace VE
