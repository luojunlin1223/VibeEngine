#include "VibeEngine/Renderer/Renderer.h"
#include "VibeEngine/Renderer/RenderCommand.h"
#include "VibeEngine/Core/Log.h"

namespace VE {

void Renderer::Init(RendererAPI::API api) {
    RendererAPI::SetAPI(api);
    RenderCommand::Init();
    VE_ENGINE_INFO("Renderer initialized with {0}",
        api == RendererAPI::API::OpenGL ? "OpenGL" : "Vulkan");
}

void Renderer::Shutdown() {
    VE_ENGINE_INFO("Renderer shutdown");
}

void Renderer::BeginFrame() {
    RenderCommand::SetClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    RenderCommand::Clear();
}

void Renderer::EndFrame() {
}

} // namespace VE
