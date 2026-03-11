#pragma once

#include <cstdint>
#include <memory>

namespace VE {

class VertexArray;

class RendererAPI {
public:
    enum class API { None = 0, OpenGL, Vulkan };
    enum class DepthFunc { Less, LessEqual };

    virtual ~RendererAPI() = default;

    virtual void Init() = 0;
    virtual void SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height) = 0;
    virtual void SetClearColor(float r, float g, float b, float a) = 0;
    virtual void Clear() = 0;
    virtual void DrawIndexed(const std::shared_ptr<VertexArray>& vertexArray) = 0;
    virtual void DrawLines(const std::shared_ptr<VertexArray>& vertexArray, uint32_t vertexCount) = 0;
    virtual void SetLineWidth(float width) = 0;
    virtual void SetDepthFunc(DepthFunc func) = 0;
    virtual void SetDepthWrite(bool enabled) = 0;

    static API GetAPI() { return s_API; }
    static void SetAPI(API api) { s_API = api; }
    static std::unique_ptr<RendererAPI> Create();

private:
    static API s_API;
};

} // namespace VE
