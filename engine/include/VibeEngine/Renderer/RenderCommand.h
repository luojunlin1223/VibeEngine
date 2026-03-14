#pragma once

#include "RendererAPI.h"
#include <memory>
#include <cstdint>

namespace VE {

struct RenderStats {
    uint32_t DrawCalls    = 0;
    uint32_t Vertices     = 0;
    uint32_t Triangles    = 0;
    uint32_t Instances    = 0;
    uint32_t SetPassCalls = 0; // shader bind count
    uint32_t VisibleObjects = 0;
    uint32_t CulledObjects  = 0;
};

class RenderCommand {
public:
    static void Init();
    static void Shutdown();
    static void SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
    static void SetClearColor(float r, float g, float b, float a);
    static void Clear();
    static void DrawIndexed(const std::shared_ptr<VertexArray>& vertexArray);
    static void DrawIndexedInstanced(const std::shared_ptr<VertexArray>& vertexArray, uint32_t instanceCount);
    static void DrawLines(const std::shared_ptr<VertexArray>& vertexArray, uint32_t vertexCount);
    static void SetLineWidth(float width);
    static void SetDepthFunc(RendererAPI::DepthFunc func);
    static void SetDepthWrite(bool enabled);

    static const RenderStats& GetStats() { return s_Stats; }
    static void ResetStats() { s_Stats = {}; }

private:
    static std::unique_ptr<RendererAPI> s_RendererAPI;
    static RenderStats s_Stats;
};

} // namespace VE
