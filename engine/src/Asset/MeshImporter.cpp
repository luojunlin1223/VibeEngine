#include "VibeEngine/Asset/MeshImporter.h"
#include "VibeEngine/Asset/FBXImporter.h"
#include "VibeEngine/Asset/GLTFImporter.h"
#include "VibeEngine/Core/Log.h"

#include <unordered_map>
#include <filesystem>
#include <algorithm>

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

std::shared_ptr<MeshAsset> MeshImporter::LoadGLTF(const std::string& absolutePath) {
    return GLTFImporter::Import(absolutePath);
}

std::shared_ptr<MeshAsset> MeshImporter::GetOrLoad(const std::string& absolutePath) {
    auto it = s_Cache.find(absolutePath);
    if (it != s_Cache.end() && it->second && it->second->VAO)
        return it->second;

    // Dispatch by file extension
    std::string ext = std::filesystem::path(absolutePath).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    std::shared_ptr<MeshAsset> asset;
    if (ext == ".gltf" || ext == ".glb") {
        asset = LoadGLTF(absolutePath);
    } else {
        asset = LoadFBX(absolutePath);
    }

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
