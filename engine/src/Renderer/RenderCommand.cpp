#include "VibeEngine/Renderer/RenderCommand.h"
#include "VibeEngine/Renderer/VertexArray.h"

namespace VE {

std::unique_ptr<RendererAPI> RenderCommand::s_RendererAPI = nullptr;
RenderStats RenderCommand::s_Stats = {};

void RenderCommand::Init() {
    s_RendererAPI = RendererAPI::Create();
    s_RendererAPI->Init();
}

void RenderCommand::Shutdown() {
    s_RendererAPI.reset();
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
    if (vertexArray && vertexArray->GetIndexBuffer()) {
        uint32_t indexCount = vertexArray->GetIndexBuffer()->GetCount();
        s_Stats.DrawCalls++;
        s_Stats.Triangles += indexCount / 3;
        s_Stats.Vertices += indexCount; // conservative: index count as drawn vertices
    }
}

void RenderCommand::DrawIndexedInstanced(const std::shared_ptr<VertexArray>& vertexArray, uint32_t instanceCount) {
    s_RendererAPI->DrawIndexedInstanced(vertexArray, instanceCount);
    if (vertexArray && vertexArray->GetIndexBuffer()) {
        uint32_t indexCount = vertexArray->GetIndexBuffer()->GetCount();
        s_Stats.DrawCalls++;
        s_Stats.Instances += instanceCount;
        s_Stats.Triangles += (indexCount / 3) * instanceCount;
        s_Stats.Vertices += indexCount * instanceCount;
    }
}

void RenderCommand::DrawLines(const std::shared_ptr<VertexArray>& vertexArray, uint32_t vertexCount) {
    s_RendererAPI->DrawLines(vertexArray, vertexCount);
    s_Stats.DrawCalls++;
    s_Stats.Vertices += vertexCount;
}

void RenderCommand::SetLineWidth(float width) {
    s_RendererAPI->SetLineWidth(width);
}

void RenderCommand::SetDepthFunc(RendererAPI::DepthFunc func) {
    s_RendererAPI->SetDepthFunc(func);
}

void RenderCommand::SetDepthWrite(bool enabled) {
    s_RendererAPI->SetDepthWrite(enabled);
}

} // namespace VE
