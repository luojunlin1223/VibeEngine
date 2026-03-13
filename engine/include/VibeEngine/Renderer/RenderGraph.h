/*
 * RenderGraph — Declarative render pass scheduling with dependency validation,
 * pass culling, and transient texture pooling.
 *
 * Usage:
 *   RenderGraph rg;
 *   rg.SetViewportSize(w, h);
 *   rg.AddPass("PassName", setupFn, execFn);
 *   rg.Compile();   // validate → cull → lifetime → pool assign
 *   rg.Execute();   // run non-culled passes in order
 *   rg.Clear();     // reset per frame (pool persists)
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace VE {

// ---------------------------------------------------------------------------
// RGHandle — opaque reference to a render graph resource
// ---------------------------------------------------------------------------
struct RGHandle {
    uint32_t Index = UINT32_MAX;
    bool IsValid() const { return Index != UINT32_MAX; }
};

// ---------------------------------------------------------------------------
// RGTextureDesc — descriptor for transient textures
// ---------------------------------------------------------------------------
struct RGTextureDesc {
    uint32_t Width  = 0;
    uint32_t Height = 0;
    bool HDR          = false;
    bool DepthStencil = false;
};

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
class RGBuilder;
class RGResources;
class RenderGraph;

// ---------------------------------------------------------------------------
// Internal resource record
// ---------------------------------------------------------------------------
struct RGResource {
    std::string Name;
    RGTextureDesc Desc;
    bool Imported       = false;
    uint32_t GLTextureID = 0;  // for imported resources
    uint32_t GLFramebufferID = 0; // for transient: auto-created FBO
    int FirstUse = -1;
    int LastUse  = -1;
    int PoolSlot = -1;
};

// ---------------------------------------------------------------------------
// Internal pass record
// ---------------------------------------------------------------------------
struct RGPass {
    std::string Name;
    std::vector<RGHandle> Reads;
    std::vector<RGHandle> Writes;
    bool HasSideEffect = false;
    bool Culled        = false;

    std::function<void(RGBuilder&)> SetupFn;
    std::function<void(const RGResources&)> ExecFn;
};

// ---------------------------------------------------------------------------
// TransientTexturePool — reuses GL textures+FBOs across frames
// ---------------------------------------------------------------------------
class TransientTexturePool {
public:
    struct Slot {
        uint32_t Texture     = 0;
        uint32_t Framebuffer = 0;
        RGTextureDesc Desc;
        bool InUse = false;
    };

    // Acquire a slot matching the descriptor; creates if none available
    int Acquire(const RGTextureDesc& desc, uint32_t w, uint32_t h);
    void Release(int slot);
    void Shutdown();

    const Slot& GetSlot(int index) const { return m_Slots[index]; }

private:
    bool IsCompatible(const Slot& s, const RGTextureDesc& desc, uint32_t w, uint32_t h) const;
    int CreateSlot(const RGTextureDesc& desc, uint32_t w, uint32_t h);

    std::vector<Slot> m_Slots;
};

// ---------------------------------------------------------------------------
// RGBuilder — used inside pass setup lambdas to declare dependencies
// ---------------------------------------------------------------------------
class RGBuilder {
public:
    RGBuilder(RenderGraph& graph, int passIndex);

    RGHandle Import(const std::string& name, uint32_t glTexID, uint32_t w, uint32_t h);
    RGHandle Create(const std::string& name, const RGTextureDesc& desc);
    RGHandle Read(RGHandle handle);
    RGHandle Write(RGHandle handle);
    void SideEffect();

private:
    RenderGraph& m_Graph;
    int m_PassIndex;
};

// ---------------------------------------------------------------------------
// RGResources — used inside pass execute lambdas to access resources
// ---------------------------------------------------------------------------
class RGResources {
public:
    RGResources(const RenderGraph& graph);

    uint32_t GetTexture(RGHandle handle) const;
    uint32_t GetFramebuffer(RGHandle handle) const;
    uint32_t GetWidth(RGHandle handle) const;
    uint32_t GetHeight(RGHandle handle) const;

private:
    const RenderGraph& m_Graph;
};

// ---------------------------------------------------------------------------
// RenderGraph — main interface
// ---------------------------------------------------------------------------
class RenderGraph {
public:
    void SetViewportSize(uint32_t w, uint32_t h);

    void AddPass(const std::string& name,
                 std::function<void(RGBuilder&)> setupFn,
                 std::function<void(const RGResources&)> execFn);

    bool Compile();   // validate → cull → lifetime → pool assign
    void Execute();   // run non-culled passes in order
    void Clear();     // reset per frame (pool persists)

    std::string DumpGraph() const;

    // Shutdown pool (call at application exit)
    void ShutdownPool();

private:
    friend class RGBuilder;
    friend class RGResources;

    RGHandle AddResource(const RGResource& res);

    std::vector<RGPass> m_Passes;
    std::vector<RGResource> m_Resources;
    uint32_t m_ViewportWidth  = 0;
    uint32_t m_ViewportHeight = 0;

    TransientTexturePool m_Pool;
};

} // namespace VE
