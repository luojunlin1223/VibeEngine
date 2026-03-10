#include "VibeEngine/Renderer/RenderCommand.h"
#include "VibeEngine/Renderer/VertexArray.h"

namespace VE {

std::unique_ptr<RendererAPI> RenderCommand::s_RendererAPI = nullptr;

void RenderCommand::Init() {
    s_RendererAPI = RendererAPI::Create();
    s_RendererAPI->Init();
}

void RenderCommand::SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    s_RendererAPI->SetViewport(x, y, width, height);
}

void RenderCommand::SetClearColor(float r, float g, float b, float a) {
    s_RendererAPI->SetClearColor(r, g, b, a);
}

void RenderCommand::Clear() {
    s_RendererAPI->Clear();
}

void RenderCommand::DrawIndexed(const std::shared_ptr<VertexArray>& vertexArray) {
    s_RendererAPI->DrawIndexed(vertexArray);
}

} // namespace VE
