#include "VibeEngine/Core/Console.h"

namespace VE {

std::vector<LogEntry> Console::s_Entries;
std::mutex Console::s_Mutex;

void Console::Init() {
    s_Entries.reserve(MAX_ENTRIES);
}

void Console::Clear() {
    std::lock_guard<std::mutex> lock(s_Mutex);
    s_Entries.clear();
}

const std::vector<LogEntry>& Console::GetEntries() {
    return s_Entries;
}

int Console::GetEntryCount() {
    return static_cast<int>(s_Entries.size());
}

void Console::AddEntry(const std::string& msg, LogLevel level, const std::string& timestamp) {
    std::lock_guard<std::mutex> lock(s_Mutex);
    if (s_Entries.size() >= MAX_ENTRIES)
        s_Entries.erase(s_Entries.begin());
    s_Entries.push_back({ msg, level, timestamp });
}

} // namespace VE
