#pragma once

#include "VibeEngine/Asset/MeshAsset.h"
#include <string>
#include <memory>
#include <unordered_map>
#include <vector>

namespace VE {

class MeshImporter {
public:
    // Load first mesh from file, returns nullptr on failure
    static std::shared_ptr<MeshAsset> LoadFBX(const std::string& absolutePath);

    // Cached load: returns existing if already loaded
    static std::shared_ptr<MeshAsset> GetOrLoad(const std::string& absolutePath);

    // Clear cache (call on renderer switch)
    static void ClearCache();

    // Re-upload all cached meshes (call after renderer switch)
    static void ReuploadCache();

private:
    static std::unordered_map<std::string, std::shared_ptr<MeshAsset>> s_Cache;
};

} // namespace VE
