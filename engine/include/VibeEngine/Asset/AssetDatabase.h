#pragma once

#include "VibeEngine/Core/UUID.h"
#include "VibeEngine/Asset/FileWatcher.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

namespace VE {

enum class AssetType {
    Unknown, Folder, Texture2D, Scene, Mesh, Shader, MaterialAsset
};

struct AssetMeta {
    UUID      Uuid;
    AssetType Type = AssetType::Unknown;
    std::string RelativePath; // relative to Assets/
};

class AssetDatabase {
public:
    void Init(const std::string& projectRoot);
    void Shutdown();
    void Update(float deltaTime);
    void Refresh();

    const AssetMeta* GetMetaByPath(const std::string& relativePath) const;
    std::vector<std::string> GetDirectoryContents(const std::string& relativeDir) const;

    bool CreateFolder(const std::string& parentRelDir, const std::string& folderName);
    bool DeleteAsset(const std::string& relativePath);

    std::string GetAbsolutePath(const std::string& relativePath) const;
    std::string GetAssetsRoot() const { return m_AssetsRoot; }

private:
    std::string m_ProjectRoot;
    std::string m_AssetsRoot;

    FileWatcher m_FileWatcher;

    std::unordered_map<std::string, AssetMeta> m_PathToMeta; // relPath -> meta

    void ScanDirectory(const std::string& absDir, const std::string& relDir);
    AssetMeta LoadOrCreateMeta(const std::string& absPath, const std::string& relPath, bool isDirectory);
    void WriteMeta(const std::string& absPath, const AssetMeta& meta);
    AssetType DeduceType(const std::string& extension) const;

    void OnFileEvents(const std::vector<FileEvent>& events);
};

} // namespace VE
