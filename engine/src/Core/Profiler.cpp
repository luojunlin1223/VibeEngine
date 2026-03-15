#include "VibeEngine/Core/Profiler.h"
#include "VibeEngine/Renderer/RenderCommand.h"

namespace VE {

// Static member definitions
Profiler::FrameStats Profiler::s_Current = {};
Profiler::TimePoint  Profiler::s_FrameStart = {};
std::unordered_map<std::string, Profiler::TimePoint> Profiler::s_SectionStarts;
std::array<float, Profiler::HISTORY_SIZE> Profiler::s_FPSHistory = {};
std::array<float, Profiler::HISTORY_SIZE> Profiler::s_FrameTimeHistory = {};
int Profiler::s_HistoryOffset = 0;
int Profiler::s_FrameCount = 0;

void Profiler::BeginFrame() {
    s_FrameStart = Clock::now();
    s_SectionStarts.clear();

    // Reset per-frame section timings
    s_Current.RenderMs  = 0.0f;
    s_Current.PhysicsMs = 0.0f;
    s_Current.ScriptsMs = 0.0f;
    s_Current.AudioMs   = 0.0f;
    s_Current.ImGuiMs   = 0.0f;
}

void Profiler::EndFrame() {
    auto now = Clock::now();
    float frameMs = std::chrono::duration<float, std::milli>(now - s_FrameStart).count();

    s_Current.FrameTimeMs = frameMs;
    s_Current.FPS = (frameMs > 0.0f) ? (1000.0f / frameMs) : 0.0f;

    // Snapshot render stats from RenderCommand
    const auto& rs = RenderCommand::GetStats();
    s_Current.DrawCalls = rs.DrawCalls;
    s_Current.Vertices  = rs.Vertices;
    s_Current.Triangles = rs.Triangles;

    // Push into ring buffer
    s_FPSHistory[s_HistoryOffset]       = s_Current.FPS;
    s_FrameTimeHistory[s_HistoryOffset] = frameMs;
    s_HistoryOffset = (s_HistoryOffset + 1) % HISTORY_SIZE;
    if (s_FrameCount < HISTORY_SIZE) s_FrameCount++;
}

void Profiler::BeginSection(const std::string& name) {
    s_SectionStarts[name] = Clock::now();
}

void Profiler::EndSection(const std::string& name) {
    auto it = s_SectionStarts.find(name);
    if (it == s_SectionStarts.end()) return;

    float ms = std::chrono::duration<float, std::milli>(Clock::now() - it->second).count();

    // Map known section names to the struct fields
    if (name == "Render")       s_Current.RenderMs  += ms;
    else if (name == "Physics") s_Current.PhysicsMs += ms;
    else if (name == "Scripts") s_Current.ScriptsMs += ms;
    else if (name == "Audio")   s_Current.AudioMs   += ms;
    else if (name == "ImGui")   s_Current.ImGuiMs   += ms;
}

void Profiler::SetEntityCount(uint32_t count) {
    s_Current.EntityCount = count;
}

float Profiler::GetAverageFPS() {
    if (s_FrameCount == 0) return 0.0f;
    float sum = 0.0f;
    for (int i = 0; i < s_FrameCount; i++)
        sum += s_FPSHistory[i];
    return sum / static_cast<float>(s_FrameCount);
}

float Profiler::GetAverageFrameTime() {
    if (s_FrameCount == 0) return 0.0f;
    float sum = 0.0f;
    for (int i = 0; i < s_FrameCount; i++)
        sum += s_FrameTimeHistory[i];
    return sum / static_cast<float>(s_FrameCount);
}

float Profiler::GetMinFrameTime() {
    if (s_FrameCount == 0) return 0.0f;
    float m = s_FrameTimeHistory[0];
    for (int i = 1; i < s_FrameCount; i++)
        if (s_FrameTimeHistory[i] < m) m = s_FrameTimeHistory[i];
    return m;
}

float Profiler::GetMaxFrameTime() {
    if (s_FrameCount == 0) return 0.0f;
    float m = s_FrameTimeHistory[0];
    for (int i = 1; i < s_FrameCount; i++)
        if (s_FrameTimeHistory[i] > m) m = s_FrameTimeHistory[i];
    return m;
}

} // namespace VE
