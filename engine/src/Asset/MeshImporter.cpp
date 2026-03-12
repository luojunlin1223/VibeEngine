#include "VibeEngine/Asset/MeshImporter.h"
#include "VibeEngine/Asset/FBXImporter.h"
#include "VibeEngine/Core/Log.h"

#include <unordered_map>
#include <filesystem>

namespace VE {

std::unordered_map<std::string, std::shared_ptr<MeshAsset>> MeshImporter::s_Cache;

std::shared_ptr<MeshAsset> MeshImporter::LoadFBX(const std::string& absolutePath) {
    // Load settings from .meta if it exists, otherwise use defaults
    FBXImportSettings settings = FBXImporter::LoadSettings(absolutePath + ".meta");
    return FBXImporter::Import(absolutePath, settings);
}

std::shared_ptr<MeshAsset> MeshImporter::LoadFBX(const std::string& absolutePath, FBXImportSettings& settings) {
    return FBXImporter::Import(absolutePath, settings);
}

std::shared_ptr<MeshAsset> MeshImporter::GetOrLoad(const std::string& absolutePath) {
    auto it = s_Cache.find(absolutePath);
    if (it != s_Cache.end() && it->second && it->second->VAO)
        return it->second;

    auto asset = LoadFBX(absolutePath);
    if (asset)
        s_Cache[absolutePath] = asset;
    return asset;
}

void MeshImporter::InvalidateCache(const std::string& absolutePath) {
    auto it = s_Cache.find(absolutePath);
    if (it != s_Cache.end()) {
        if (it->second) it->second->Release();
        s_Cache.erase(it);
    }
}

void MeshImporter::ClearCache() {
    for (auto& [path, asset] : s_Cache) {
        if (asset) asset->Release();
    }
    // Don't erase - keep CPU data for re-upload
}

void MeshImporter::ReuploadCache() {
    for (auto& [path, asset] : s_Cache) {
        if (asset && !asset->VAO)
            asset->Upload();
    }
}

} // namespace VE
