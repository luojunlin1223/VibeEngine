#pragma once

#include "VibeEngine/Renderer/RendererAPI.h"

namespace VE {

class OpenGLRendererAPI : public RendererAPI {
public:
    void Init() override;
    void SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height) override;
    void SetClearColor(float r, float g, float b, float a) override;
    void Clear() override;
    void DrawIndexed(const std::shared_ptr<VertexArray>& vertexArray) override;
    void DrawLines(const std::shared_ptr<VertexArray>& vertexArray, uint32_t vertexCount) override;
    void SetLineWidth(float width) override;
    void SetDepthFunc(DepthFunc func) override;
    void SetDepthWrite(bool enabled) override;
};

} // namespace VE
