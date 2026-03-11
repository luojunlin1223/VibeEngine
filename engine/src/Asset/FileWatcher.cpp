#include "VibeEngine/Asset/FileWatcher.h"

namespace VE {

FileWatcher::FileWatcher(const std::string& rootPath, float pollIntervalSec) {
    Init(rootPath, pollIntervalSec);
}

void FileWatcher::Init(const std::string& rootPath, float pollIntervalSec) {
    m_RootPath = rootPath;
    m_PollInterval = pollIntervalSec;
    m_TimeSinceLastPoll = 0.0f;
    m_Snapshot.clear();

    // Build initial snapshot
    if (!std::filesystem::exists(m_RootPath)) return;

    for (auto& entry : std::filesystem::recursive_directory_iterator(m_RootPath)) {
        std::string rel = std::filesystem::relative(entry.path(), m_RootPath).generic_string();
        // Skip .meta files from the watch
        if (rel.size() > 5 && rel.substr(rel.size() - 5) == ".meta") continue;

        FileInfo info;
        info.IsDirectory = entry.is_directory();
        info.WriteTime = entry.is_directory()
            ? std::filesystem::file_time_type{}
            : entry.last_write_time();
        m_Snapshot[rel] = info;
    }
}

void FileWatcher::Update(float deltaTime) {
    if (m_RootPath.empty()) return;
    m_TimeSinceLastPoll += deltaTime;
    if (m_TimeSinceLastPoll < m_PollInterval) return;
    m_TimeSinceLastPoll = 0.0f;
    Poll();
}

void FileWatcher::Poll() {
    if (!std::filesystem::exists(m_RootPath)) return;

    std::vector<FileEvent> events;
    std::unordered_map<std::string, FileInfo> newSnapshot;

    for (auto& entry : std::filesystem::recursive_directory_iterator(m_RootPath)) {
        std::string rel = std::filesystem::relative(entry.path(), m_RootPath).generic_string();
        if (rel.size() > 5 && rel.substr(rel.size() - 5) == ".meta") continue;

        FileInfo info;
        info.IsDirectory = entry.is_directory();
        info.WriteTime = entry.is_directory()
            ? std::filesystem::file_time_type{}
            : entry.last_write_time();
        newSnapshot[rel] = info;

        auto it = m_Snapshot.find(rel);
        if (it == m_Snapshot.end()) {
            events.push_back({ FileEvent::Type::Created, rel, info.IsDirectory });
        } else if (!info.IsDirectory && info.WriteTime != it->second.WriteTime) {
            events.push_back({ FileEvent::Type::Modified, rel, false });
        }
    }

    // Check for deletions
    for (auto& [path, info] : m_Snapshot) {
        if (newSnapshot.find(path) == newSnapshot.end()) {
            events.push_back({ FileEvent::Type::Deleted, path, info.IsDirectory });
        }
    }

    m_Snapshot = std::move(newSnapshot);

    if (!events.empty() && m_Callback) {
        m_Callback(events);
    }
}

} // namespace VE
