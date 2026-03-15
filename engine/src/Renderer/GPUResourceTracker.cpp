/*
 * GPUResourceTracker implementation — Debug builds only.
 *
 * Tracks GPU resource IDs by type and reports any that remain
 * allocated at shutdown, helping catch resource leaks.
 */
#ifndef NDEBUG

#include "VibeEngine/Renderer/GPUResourceTracker.h"
#include "VibeEngine/Core/Log.h"

#include <sstream>
#include <vector>
#include <algorithm>

namespace VE {

GPUResourceTracker& GPUResourceTracker::Get() {
    static GPUResourceTracker instance;
    return instance;
}

void GPUResourceTracker::Track(GPUResourceType type, uint32_t id) {
    if (id == 0) return; // GL id 0 is not a valid user resource
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Resources[type].insert(id);
}

void GPUResourceTracker::Untrack(GPUResourceType type, uint32_t id) {
    if (id == 0) return;
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Resources.find(type);
    if (it != m_Resources.end()) {
        it->second.erase(id);
        if (it->second.empty())
            m_Resources.erase(it);
    }
}

bool GPUResourceTracker::ReportLeaks() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    bool hasLeaks = false;

    for (const auto& [type, ids] : m_Resources) {
        if (ids.empty()) continue;
        hasLeaks = true;

        // Sort IDs for deterministic output
        std::vector<uint32_t> sorted(ids.begin(), ids.end());
        std::sort(sorted.begin(), sorted.end());

        std::ostringstream oss;
        for (size_t i = 0; i < sorted.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << sorted[i];
        }

        VE_ENGINE_ERROR("[GPU Leak] {} {}s leaked (IDs: {})",
                        sorted.size(), TypeName(type), oss.str());
    }

    if (!hasLeaks)
        VE_ENGINE_INFO("[GPU Tracker] No resource leaks detected");

    return hasLeaks;
}

void GPUResourceTracker::Reset() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Resources.clear();
}

const char* GPUResourceTracker::TypeName(GPUResourceType type) {
    switch (type) {
        case GPUResourceType::Texture:       return "Texture";
        case GPUResourceType::Framebuffer:   return "Framebuffer";
        case GPUResourceType::VertexArray:   return "VertexArray";
        case GPUResourceType::VertexBuffer:  return "VertexBuffer";
        case GPUResourceType::IndexBuffer:   return "IndexBuffer";
        case GPUResourceType::ShaderProgram: return "ShaderProgram";
        default:                             return "Unknown";
    }
}

} // namespace VE

#endif // NDEBUG
