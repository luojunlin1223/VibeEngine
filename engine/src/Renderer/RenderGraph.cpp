/*
 * RenderGraph — Implementation of declarative render pass scheduling.
 *
 * Compile() performs four stages:
 *   1. Validate — each Read references a resource written by an earlier pass
 *   2. Cull    — reverse flood-fill from SideEffect passes; remove unreachable
 *   3. Lifetime — compute FirstUse / LastUse for transient resources
 *   4. Pool    — greedy slot allocation, release after LastUse
 *
 * Execute() runs non-culled passes in submission order.
 */

#include <VibeEngine/Renderer/RenderGraph.h>
#include <VibeEngine/Core/Log.h>
#include <glad/gl.h>

#include <sstream>
#include <algorithm>

namespace VE {

// ===========================================================================
// TransientTexturePool
// ===========================================================================

bool TransientTexturePool::IsCompatible(const Slot& s, const RGTextureDesc& desc,
                                         uint32_t w, uint32_t h) const {
    return !s.InUse
        && s.Desc.Width  == w
        && s.Desc.Height == h
        && s.Desc.HDR    == desc.HDR
        && s.Desc.DepthStencil == desc.DepthStencil;
}

int TransientTexturePool::Acquire(const RGTextureDesc& desc, uint32_t w, uint32_t h) {
    // Try to find a compatible free slot
    for (size_t i = 0; i < m_Slots.size(); i++) {
        if (IsCompatible(m_Slots[i], desc, w, h)) {
            m_Slots[i].InUse = true;
            return static_cast<int>(i);
        }
    }
    return CreateSlot(desc, w, h);
}

void TransientTexturePool::Release(int slot) {
    if (slot >= 0 && slot < static_cast<int>(m_Slots.size()))
        m_Slots[slot].InUse = false;
}

int TransientTexturePool::CreateSlot(const RGTextureDesc& desc, uint32_t w, uint32_t h) {
    Slot s;
    s.Desc = desc;
    s.Desc.Width  = w;
    s.Desc.Height = h;
    s.InUse = true;

    // Create GL texture
    glGenTextures(1, &s.Texture);
    glBindTexture(GL_TEXTURE_2D, s.Texture);

    if (desc.DepthStencil) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, w, h, 0,
                     GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
    } else {
        GLenum internalFmt = desc.HDR ? GL_RGBA16F : GL_RGBA8;
        GLenum type        = desc.HDR ? GL_FLOAT   : GL_UNSIGNED_BYTE;
        glTexImage2D(GL_TEXTURE_2D, 0, internalFmt, w, h, 0, GL_RGBA, type, nullptr);
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Create FBO
    glGenFramebuffers(1, &s.Framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, s.Framebuffer);

    if (desc.DepthStencil) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                               GL_TEXTURE_2D, s.Texture, 0);
    } else {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, s.Texture, 0);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    int index = static_cast<int>(m_Slots.size());
    m_Slots.push_back(s);
    return index;
}

void TransientTexturePool::Shutdown() {
    for (auto& s : m_Slots) {
        if (s.Framebuffer) glDeleteFramebuffers(1, &s.Framebuffer);
        if (s.Texture)     glDeleteTextures(1, &s.Texture);
        s.Framebuffer = 0;
        s.Texture     = 0;
    }
    m_Slots.clear();
}

// ===========================================================================
// RGBuilder
// ===========================================================================

RGBuilder::RGBuilder(RenderGraph& graph, int passIndex)
    : m_Graph(graph), m_PassIndex(passIndex) {}

RGHandle RGBuilder::Import(const std::string& name, uint32_t glTexID, uint32_t w, uint32_t h) {
    // Check if already imported with same name
    for (size_t i = 0; i < m_Graph.m_Resources.size(); i++) {
        if (m_Graph.m_Resources[i].Imported && m_Graph.m_Resources[i].Name == name) {
            return RGHandle{ static_cast<uint32_t>(i) };
        }
    }

    RGResource res;
    res.Name        = name;
    res.Imported    = true;
    res.GLTextureID = glTexID;
    res.Desc.Width  = w;
    res.Desc.Height = h;
    return m_Graph.AddResource(res);
}

RGHandle RGBuilder::Create(const std::string& name, const RGTextureDesc& desc) {
    RGResource res;
    res.Name     = name;
    res.Imported = false;
    res.Desc     = desc;
    return m_Graph.AddResource(res);
}

RGHandle RGBuilder::Read(RGHandle handle) {
    if (handle.IsValid())
        m_Graph.m_Passes[m_PassIndex].Reads.push_back(handle);
    return handle;
}

RGHandle RGBuilder::Write(RGHandle handle) {
    if (handle.IsValid())
        m_Graph.m_Passes[m_PassIndex].Writes.push_back(handle);
    return handle;
}

void RGBuilder::SideEffect() {
    m_Graph.m_Passes[m_PassIndex].HasSideEffect = true;
}

// ===========================================================================
// RGResources
// ===========================================================================

RGResources::RGResources(const RenderGraph& graph)
    : m_Graph(graph) {}

uint32_t RGResources::GetTexture(RGHandle handle) const {
    if (!handle.IsValid() || handle.Index >= m_Graph.m_Resources.size())
        return 0;
    const auto& res = m_Graph.m_Resources[handle.Index];
    if (res.Imported)
        return res.GLTextureID;
    if (res.PoolSlot >= 0)
        return m_Graph.m_Pool.GetSlot(res.PoolSlot).Texture;
    return 0;
}

uint32_t RGResources::GetFramebuffer(RGHandle handle) const {
    if (!handle.IsValid() || handle.Index >= m_Graph.m_Resources.size())
        return 0;
    const auto& res = m_Graph.m_Resources[handle.Index];
    if (res.Imported)
        return 0; // imported resources use their own FBO
    if (res.PoolSlot >= 0)
        return m_Graph.m_Pool.GetSlot(res.PoolSlot).Framebuffer;
    return 0;
}

uint32_t RGResources::GetWidth(RGHandle handle) const {
    if (!handle.IsValid() || handle.Index >= m_Graph.m_Resources.size())
        return 0;
    const auto& res = m_Graph.m_Resources[handle.Index];
    if (res.Desc.Width > 0) return res.Desc.Width;
    return m_Graph.m_ViewportWidth;
}

uint32_t RGResources::GetHeight(RGHandle handle) const {
    if (!handle.IsValid() || handle.Index >= m_Graph.m_Resources.size())
        return 0;
    const auto& res = m_Graph.m_Resources[handle.Index];
    if (res.Desc.Height > 0) return res.Desc.Height;
    return m_Graph.m_ViewportHeight;
}

// ===========================================================================
// RenderGraph
// ===========================================================================

void RenderGraph::SetViewportSize(uint32_t w, uint32_t h) {
    m_ViewportWidth  = w;
    m_ViewportHeight = h;
}

RGHandle RenderGraph::AddResource(const RGResource& res) {
    RGHandle h;
    h.Index = static_cast<uint32_t>(m_Resources.size());
    m_Resources.push_back(res);
    return h;
}

void RenderGraph::AddPass(const std::string& name,
                           std::function<void(RGBuilder&)> setupFn,
                           std::function<void(const RGResources&)> execFn) {
    int passIdx = static_cast<int>(m_Passes.size());
    RGPass pass;
    pass.Name    = name;
    pass.SetupFn = std::move(setupFn);
    pass.ExecFn  = std::move(execFn);
    m_Passes.push_back(std::move(pass));

    // Run setup to populate reads/writes/side-effects
    RGBuilder builder(*this, passIdx);
    m_Passes[passIdx].SetupFn(builder);
}

bool RenderGraph::Compile() {
    // ── Stage 1: Validate ──
    // For each read, check that the resource was written by an earlier pass
    for (int i = 0; i < static_cast<int>(m_Passes.size()); i++) {
        for (auto& rh : m_Passes[i].Reads) {
            if (!rh.IsValid()) continue;
            const auto& res = m_Resources[rh.Index];
            if (res.Imported) continue; // imported resources are always valid

            bool foundWriter = false;
            for (int j = 0; j < i; j++) {
                for (auto& wh : m_Passes[j].Writes) {
                    if (wh.Index == rh.Index) {
                        foundWriter = true;
                        break;
                    }
                }
                if (foundWriter) break;
            }
            if (!foundWriter) {
                VE_ERROR("RenderGraph: Pass '{}' reads resource '{}' which has no prior writer",
                         m_Passes[i].Name, res.Name);
                return false;
            }
        }
    }

    // ── Stage 2: Cull ──
    // Reverse flood-fill from SideEffect passes
    std::vector<bool> alive(m_Passes.size(), false);
    for (int i = static_cast<int>(m_Passes.size()) - 1; i >= 0; i--) {
        if (m_Passes[i].HasSideEffect)
            alive[i] = true;
    }

    // Propagate: if pass i is alive and reads resource R, mark writers of R as alive
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < static_cast<int>(m_Passes.size()); i++) {
            if (!alive[i]) continue;
            for (auto& rh : m_Passes[i].Reads) {
                if (!rh.IsValid()) continue;
                for (int j = 0; j < i; j++) {
                    if (alive[j]) continue;
                    for (auto& wh : m_Passes[j].Writes) {
                        if (wh.Index == rh.Index) {
                            alive[j] = true;
                            changed = true;
                        }
                    }
                }
            }
        }
    }

    for (size_t i = 0; i < m_Passes.size(); i++)
        m_Passes[i].Culled = !alive[i];

    // ── Stage 3: Lifetime ──
    for (size_t i = 0; i < m_Passes.size(); i++) {
        if (m_Passes[i].Culled) continue;
        int pi = static_cast<int>(i);
        for (auto& rh : m_Passes[i].Reads) {
            if (!rh.IsValid()) continue;
            auto& res = m_Resources[rh.Index];
            if (res.FirstUse < 0) res.FirstUse = pi;
            res.LastUse = pi;
        }
        for (auto& wh : m_Passes[i].Writes) {
            if (!wh.IsValid()) continue;
            auto& res = m_Resources[wh.Index];
            if (res.FirstUse < 0) res.FirstUse = pi;
            res.LastUse = pi;
        }
    }

    // ── Stage 4: Pool Assign (transient resources only) ──
    for (size_t i = 0; i < m_Resources.size(); i++) {
        auto& res = m_Resources[i];
        if (res.Imported || res.FirstUse < 0) continue;

        uint32_t w = res.Desc.Width  > 0 ? res.Desc.Width  : m_ViewportWidth;
        uint32_t h = res.Desc.Height > 0 ? res.Desc.Height : m_ViewportHeight;
        res.PoolSlot = m_Pool.Acquire(res.Desc, w, h);
    }

    return true;
}

void RenderGraph::Execute() {
    RGResources resources(*this);
    for (size_t i = 0; i < m_Passes.size(); i++) {
        if (m_Passes[i].Culled) continue;
        m_Passes[i].ExecFn(resources);
    }

    // Release transient pool slots
    for (auto& res : m_Resources) {
        if (!res.Imported && res.PoolSlot >= 0)
            m_Pool.Release(res.PoolSlot);
    }
}

void RenderGraph::Clear() {
    m_Passes.clear();
    m_Resources.clear();
}

void RenderGraph::ShutdownPool() {
    m_Pool.Shutdown();
}

std::string RenderGraph::DumpGraph() const {
    std::ostringstream ss;
    ss << "=== RenderGraph (" << m_Passes.size() << " passes, "
       << m_Resources.size() << " resources) ===\n";

    for (size_t i = 0; i < m_Passes.size(); i++) {
        const auto& pass = m_Passes[i];
        ss << "  Pass " << i << ": \"" << pass.Name << "\"";
        if (pass.Culled)        ss << " [CULLED]";
        if (pass.HasSideEffect) ss << " [SideEffect]";
        ss << "\n";

        if (!pass.Reads.empty()) {
            ss << "    Reads: ";
            for (auto& rh : pass.Reads) {
                if (rh.IsValid() && rh.Index < m_Resources.size())
                    ss << "\"" << m_Resources[rh.Index].Name << "\" ";
            }
            ss << "\n";
        }
        if (!pass.Writes.empty()) {
            ss << "    Writes: ";
            for (auto& wh : pass.Writes) {
                if (wh.IsValid() && wh.Index < m_Resources.size())
                    ss << "\"" << m_Resources[wh.Index].Name << "\" ";
            }
            ss << "\n";
        }
    }

    ss << "  Resources:\n";
    for (size_t i = 0; i < m_Resources.size(); i++) {
        const auto& res = m_Resources[i];
        ss << "    [" << i << "] \"" << res.Name << "\" "
           << (res.Imported ? "IMPORTED" : "TRANSIENT")
           << " life=[" << res.FirstUse << "," << res.LastUse << "]";
        if (res.PoolSlot >= 0) ss << " pool=" << res.PoolSlot;
        ss << "\n";
    }

    return ss.str();
}

} // namespace VE
