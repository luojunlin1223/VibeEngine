/*
 * Console — In-editor log viewer with filtering and search.
 *
 * Captures spdlog output and stores recent messages for display
 * in an ImGui panel. Supports filtering by level and text search.
 */
#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <spdlog/sinks/base_sink.h>

namespace VE {

enum class LogLevel { Trace, Info, Warn, Error };

struct LogEntry {
    std::string Message;
    LogLevel    Level;
    std::string Timestamp;
};

class Console {
public:
    static void Init();
    static void Clear();

    static const std::vector<LogEntry>& GetEntries();
    static int GetEntryCount();

    // Thread-safe add (called from spdlog sink)
    static void AddEntry(const std::string& msg, LogLevel level, const std::string& timestamp);

    static constexpr int MAX_ENTRIES = 2000;

private:
    static std::vector<LogEntry> s_Entries;
    static std::mutex s_Mutex;
};

// Custom spdlog sink that feeds into Console
template<typename Mutex>
class ConsoleSink : public spdlog::sinks::base_sink<Mutex> {
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t formatted;
        spdlog::sinks::base_sink<Mutex>::formatter_->format(msg, formatted);

        LogLevel level = LogLevel::Info;
        switch (msg.level) {
            case spdlog::level::trace: level = LogLevel::Trace; break;
            case spdlog::level::debug:
            case spdlog::level::info:  level = LogLevel::Info; break;
            case spdlog::level::warn:  level = LogLevel::Warn; break;
            case spdlog::level::err:
            case spdlog::level::critical: level = LogLevel::Error; break;
            default: break;
        }

        // Extract timestamp from formatted message (first N chars)
        std::string full(formatted.data(), formatted.size());
        Console::AddEntry(full, level, "");
    }

    void flush_() override {}
};

using ConsoleSink_mt = ConsoleSink<std::mutex>;

} // namespace VE
