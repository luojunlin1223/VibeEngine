#include "VibeEngine/Asset/AssetDatabase.h"
#include "VibeEngine/Core/Log.h"

#include <yaml-cpp/yaml.h>
#include <fstream>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

namespace VE {

void AssetDatabase::Init(const std::string& projectRoot) {
    m_ProjectRoot = projectRoot;
    m_AssetsRoot = (fs::path(projectRoot) / "Assets").generic_string();

    // Ensure directories exist
    fs::create_directories(m_AssetsRoot);
    fs::create_directories((fs::path(projectRoot) / "ProjectSettings").generic_string());

    Refresh();

    m_FileWatcher.Init(m_AssetsRoot, 1.0f);
    m_FileWatcher.SetCallback([this](const std::vector<FileEvent>& events) {
        OnFileEvents(events);
    });

    VE_ENGINE_INFO("AssetDatabase initialized: {0}", m_AssetsRoot);
}

void AssetDatabase::Shutdown() {
    m_PathToMeta.clear();
}

void AssetDatabase::Update(float deltaTime) {
    m_FileWatcher.Update(deltaTime);
}

void AssetDatabase::Refresh() {
    m_PathToMeta.clear();
    if (fs::exists(m_AssetsRoot))
        ScanDirectory(m_AssetsRoot, "");
}

void AssetDatabase::ScanDirectory(const std::string& absDir, const std::string& relDir) {
    for (auto& entry : fs::directory_iterator(absDir)) {
        std::string name = entry.path().filename().generic_string();

        // Skip .meta files
        if (name.size() > 5 && name.substr(name.size() - 5) == ".meta")
            continue;

        std::string relPath = relDir.empty() ? name : relDir + "/" + name;
        std::string absPath = entry.path().generic_string();
        bool isDir = entry.is_directory();

        AssetMeta meta = LoadOrCreateMeta(absPath, relPath, isDir);
        m_PathToMeta[relPath] = meta;

        if (isDir)
            ScanDirectory(absPath, relPath);
    }
}

AssetMeta AssetDatabase::LoadOrCreateMeta(const std::string& absPath,
                                           const std::string& relPath,
                                           bool isDirectory) {
    std::string metaPath = absPath + ".meta";
    AssetMeta meta;
    meta.RelativePath = relPath;

    if (fs::exists(metaPath)) {
        try {
            YAML::Node data = YAML::LoadFile(metaPath);
            if (data["uuid"])
                meta.Uuid = UUID(data["uuid"].as<uint64_t>());
            if (data["type"]) {
                std::string t = data["type"].as<std::string>();
                if (t == "Folder") meta.Type = AssetType::Folder;
                else if (t == "Texture2D") meta.Type = AssetType::Texture2D;
                else if (t == "Scene") meta.Type = AssetType::Scene;
                else if (t == "Mesh") meta.Type = AssetType::Mesh;
                else if (t == "Shader") meta.Type = AssetType::Shader;
                else if (t == "Material") meta.Type = AssetType::MaterialAsset;
                else meta.Type = AssetType::Unknown;
            }
            return meta;
        } catch (...) {
            // Corrupted meta, regenerate
        }
    }

    // Generate new meta
    meta.Uuid = UUID();
    if (isDirectory) {
        meta.Type = AssetType::Folder;
    } else {
        std::string ext = fs::path(absPath).extension().generic_string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        meta.Type = DeduceType(ext);
    }

    WriteMeta(absPath, meta);
    return meta;
}

void AssetDatabase::WriteMeta(const std::string& absPath, const AssetMeta& meta) {
    std::string metaPath = absPath + ".meta";
    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "uuid" << YAML::Value << static_cast<uint64_t>(meta.Uuid);
    const char* typeStr = "Unknown";
    switch (meta.Type) {
        case AssetType::Folder:    typeStr = "Folder"; break;
        case AssetType::Texture2D: typeStr = "Texture2D"; break;
        case AssetType::Scene:     typeStr = "Scene"; break;
        case AssetType::Mesh:          typeStr = "Mesh"; break;
        case AssetType::Shader:        typeStr = "Shader"; break;
        case AssetType::MaterialAsset: typeStr = "Material"; break;
        default: break;
    }
    out << YAML::Key << "type" << YAML::Value << typeStr;
    out << YAML::EndMap;

    std::ofstream fout(metaPath);
    fout << out.c_str();
}

AssetType AssetDatabase::DeduceType(const std::string& extension) const {
    if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
        extension == ".bmp" || extension == ".tga" || extension == ".hdr")
        return AssetType::Texture2D;
    if (extension == ".vscene")
        return AssetType::Scene;
    if (extension == ".fbx" || extension == ".obj")
        return AssetType::Mesh;
    if (extension == ".shader" || extension == ".glsl")
        return AssetType::Shader;
    if (extension == ".vmat")
        return AssetType::MaterialAsset;
    return AssetType::Unknown;
}

const AssetMeta* AssetDatabase::GetMetaByPath(const std::string& relativePath) const {
    auto it = m_PathToMeta.find(relativePath);
    if (it != m_PathToMeta.end()) return &it->second;
    return nullptr;
}

std::vector<std::string> AssetDatabase::GetDirectoryContents(const std::string& relativeDir) const {
    std::vector<std::string> result;
    std::string prefix = relativeDir.empty() ? "" : relativeDir + "/";

    for (auto& [path, meta] : m_PathToMeta) {
        // Check if this is a direct child of relativeDir
        if (path.size() <= prefix.size()) continue;
        if (!prefix.empty() && path.substr(0, prefix.size()) != prefix) continue;

        std::string remainder = path.substr(prefix.size());
        // Direct child = no more slashes
        if (remainder.find('/') == std::string::npos)
            result.push_back(path);
    }

    // Sort: folders first, then alphabetically
    std::sort(result.begin(), result.end(), [this](const std::string& a, const std::string& b) {
        auto* ma = GetMetaByPath(a);
        auto* mb = GetMetaByPath(b);
        bool aDir = ma && ma->Type == AssetType::Folder;
        bool bDir = mb && mb->Type == AssetType::Folder;
        if (aDir != bDir) return aDir > bDir;
        return a < b;
    });

    return result;
}

bool AssetDatabase::CreateFolder(const std::string& parentRelDir, const std::string& folderName) {
    std::string relPath = parentRelDir.empty() ? folderName : parentRelDir + "/" + folderName;
    std::string absPath = (fs::path(m_AssetsRoot) / relPath).generic_string();

    if (fs::exists(absPath)) return false;

    fs::create_directories(absPath);

    AssetMeta meta;
    meta.Uuid = UUID();
    meta.Type = AssetType::Folder;
    meta.RelativePath = relPath;
    WriteMeta(absPath, meta);
    m_PathToMeta[relPath] = meta;

    VE_ENGINE_INFO("Created folder: Assets/{0}", relPath);
    return true;
}

bool AssetDatabase::DeleteAsset(const std::string& relativePath) {
    std::string absPath = (fs::path(m_AssetsRoot) / relativePath).generic_string();
    std::string metaPath = absPath + ".meta";

    if (!fs::exists(absPath)) return false;

    // If it's a directory, remove all children from the database
    if (fs::is_directory(absPath)) {
        std::string prefix = relativePath + "/";
        std::vector<std::string> toRemove;
        for (auto& [path, meta] : m_PathToMeta) {
            if (path == relativePath || path.substr(0, prefix.size()) == prefix)
                toRemove.push_back(path);
        }
        for (auto& p : toRemove)
            m_PathToMeta.erase(p);

        fs::remove_all(absPath);
    } else {
        m_PathToMeta.erase(relativePath);
        fs::remove(absPath);
    }

    if (fs::exists(metaPath))
        fs::remove(metaPath);

    VE_ENGINE_INFO("Deleted: Assets/{0}", relativePath);
    return true;
}

std::string AssetDatabase::GetAbsolutePath(const std::string& relativePath) const {
    return (fs::path(m_AssetsRoot) / relativePath).generic_string();
}

void AssetDatabase::OnFileEvents(const std::vector<FileEvent>& events) {
    bool needRefresh = false;
    for (auto& e : events) {
        VE_ENGINE_INFO("Asset {0}: {1}",
            e.EventType == FileEvent::Type::Created ? "created" :
            e.EventType == FileEvent::Type::Modified ? "modified" : "deleted",
            e.FilePath);
        needRefresh = true;
    }
    if (needRefresh)
        Refresh();
}

} // namespace VE
