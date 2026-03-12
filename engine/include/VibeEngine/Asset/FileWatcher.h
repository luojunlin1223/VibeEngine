#pragma once

#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <filesystem>

namespace VE {

struct FileEvent {
    enum class Type { Created, Modified, Deleted };
    Type EventType;
    std::string FilePath; // relative to watch root
    bool IsDirectory;
};

class FileWatcher {
public:
    FileWatcher() = default;
    FileWatcher(const std::string& rootPath, float pollIntervalSec = 1.0f);

    void Init(const std::string& rootPath, float pollIntervalSec = 1.0f);
    void Update(float deltaTime);

    using Callback = std::function<void(const std::vector<FileEvent>&)>;
    void SetCallback(Callback cb) { m_Callback = std::move(cb); }

private:
    void Poll();

    std::string m_RootPath;
    float m_PollInterval = 1.0f;
    float m_TimeSinceLastPoll = 0.0f;
    Callback m_Callback;

    struct FileInfo {
        std::filesystem::file_time_type WriteTime;
        bool IsDirectory;
    };
    std::unordered_map<std::string, FileInfo> m_Snapshot;
};

} // namespace VE
