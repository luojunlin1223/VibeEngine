#pragma once

#include "RendererAPI.h"
#include <memory>

namespace VE {

class RenderCommand {
public:
    static void Init();
    static void Shutdown();
    static void SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
    static void SetClearColor(float r, float g, float b, float a);
    static void Clear();
    static void DrawIndexed(const std::shared_ptr<VertexArray>& vertexArray);
    static void DrawLines(const std::shared_ptr<VertexArray>& vertexArray, uint32_t vertexCount);
    static void SetLineWidth(float width);
    static void SetDepthFunc(RendererAPI::DepthFunc func);
    static void SetDepthWrite(bool enabled);

private:
    static std::unique_ptr<RendererAPI> s_RendererAPI;
};

} // namespace VE
