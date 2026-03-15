/*
 * GPUResourceTracker — Debug-only singleton that tracks OpenGL resource
 * allocations (textures, framebuffers, VAOs, VBOs, IBOs, shader programs).
 *
 * Track() is called when a resource is created, Untrack() when destroyed.
 * At shutdown, ReportLeaks() logs any resources that were not properly freed.
 *
 * All calls compile to no-ops in Release builds (NDEBUG defined).
 */
#pragma once

#ifndef NDEBUG

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>

namespace VE {

enum class GPUResourceType {
    Texture,
    Framebuffer,
    VertexArray,
    VertexBuffer,
    IndexBuffer,
    ShaderProgram
};

class GPUResourceTracker {
public:
    static GPUResourceTracker& Get();

    /// Record creation of a GPU resource.
    void Track(GPUResourceType type, uint32_t id);

    /// Record destruction of a GPU resource.
    void Untrack(GPUResourceType type, uint32_t id);

    /// Log any resources that have not been untracked. Returns true if leaks found.
    bool ReportLeaks();

    /// Reset all tracking state (e.g. after renderer switch).
    void Reset();

private:
    GPUResourceTracker() = default;

    static const char* TypeName(GPUResourceType type);

    std::mutex m_Mutex;
    std::unordered_map<GPUResourceType, std::unordered_set<uint32_t>> m_Resources;
};

// ── Convenience macros ──────────────────────────────────────────────────
#define VE_GPU_TRACK(type, id)   ::VE::GPUResourceTracker::Get().Track(type, id)
#define VE_GPU_UNTRACK(type, id) ::VE::GPUResourceTracker::Get().Untrack(type, id)
#define VE_GPU_REPORT_LEAKS()    ::VE::GPUResourceTracker::Get().ReportLeaks()
#define VE_GPU_RESET_TRACKER()   ::VE::GPUResourceTracker::Get().Reset()

} // namespace VE

#else // NDEBUG — Release builds: zero overhead

#define VE_GPU_TRACK(type, id)   ((void)0)
#define VE_GPU_UNTRACK(type, id) ((void)0)
#define VE_GPU_REPORT_LEAKS()    ((void)0)
#define VE_GPU_RESET_TRACKER()   ((void)0)

#endif // NDEBUG
