/*
 * Profiler — Lightweight frame profiling and debug statistics.
 *
 * Tracks per-frame timing for subsystems (Render, Physics, Scripts, Audio, ImGui),
 * FPS / frame time history, and aggregates RenderCommand stats. Uses RAII scoped
 * timers so instrumenting a code section is a single macro.
 *
 * Usage:
 *   PROFILE_SCOPE("Render");          // times until scope exit
 *   Profiler::BeginFrame();           // call once at top of frame
 *   Profiler::EndFrame();             // call once at bottom of frame
 */
#pragma once

#include <chrono>
#include <string>
#include <unordered_map>
#include <array>
#include <cstdint>

namespace VE {

class Profiler {
public:
    // Maximum number of frames kept in the rolling history
    static constexpr int HISTORY_SIZE = 256;

    struct FrameStats {
        float FrameTimeMs   = 0.0f;   // total frame time in ms
        float FPS            = 0.0f;

        // Per-section timings (ms)
        float RenderMs       = 0.0f;
        float PhysicsMs      = 0.0f;
        float ScriptsMs      = 0.0f;
        float AudioMs        = 0.0f;
        float ImGuiMs        = 0.0f;

        // Render stats (snapshot from RenderCommand)
        uint32_t DrawCalls   = 0;
        uint32_t Vertices    = 0;
        uint32_t Triangles   = 0;
        uint32_t EntityCount = 0;
    };

    // Call at the very start of each frame
    static void BeginFrame();

    // Call at the very end of each frame (computes final stats)
    static void EndFrame();

    // Start / stop a named section timer.  Prefer PROFILE_SCOPE macro instead.
    static void BeginSection(const std::string& name);
    static void EndSection(const std::string& name);

    // Set entity count for the current frame (call after scene update)
    static void SetEntityCount(uint32_t count);

    // ── Accessors ─────────────────────────────────────────────────────
    static const FrameStats& GetCurrentStats() { return s_Current; }

    // Rolling FPS history for PlotLines
    static const float* GetFPSHistory()       { return s_FPSHistory.data(); }
    static const float* GetFrameTimeHistory() { return s_FrameTimeHistory.data(); }
    static int          GetHistorySize()      { return HISTORY_SIZE; }
    static int          GetHistoryOffset()    { return s_HistoryOffset; }

    // Aggregated stats over the history window
    static float GetAverageFPS();
    static float GetAverageFrameTime();
    static float GetMinFrameTime();
    static float GetMaxFrameTime();

private:
    using Clock = std::chrono::high_resolution_clock;
    using TimePoint = Clock::time_point;

    static FrameStats s_Current;
    static TimePoint  s_FrameStart;

    // Per-section start times (keyed by name)
    static std::unordered_map<std::string, TimePoint> s_SectionStarts;

    // Rolling history ring-buffer
    static std::array<float, HISTORY_SIZE> s_FPSHistory;
    static std::array<float, HISTORY_SIZE> s_FrameTimeHistory;
    static int s_HistoryOffset;
    static int s_FrameCount;
};

// ── RAII Scoped Timer ────────────────────────────────────────────────
class ProfileScope {
public:
    explicit ProfileScope(const std::string& name) : m_Name(name) {
        Profiler::BeginSection(m_Name);
    }
    ~ProfileScope() {
        Profiler::EndSection(m_Name);
    }
    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;
private:
    std::string m_Name;
};

#define PROFILE_SCOPE(name) ::VE::ProfileScope _profileScope##__LINE__(name)

} // namespace VE
