#include "VibeEngine/Core/Application.h"
#include "VibeEngine/Core/Window.h"
#include "VibeEngine/Core/Log.h"
#include "VibeEngine/Renderer/Renderer.h"
#include "VibeEngine/Renderer/RenderCommand.h"

namespace VE {

Application::Application(RendererAPI::API api) {
    Log::Init();
    VE_ENGINE_INFO("VibeEngine v0.1.0 initializing...");

    // Set API type first (Window needs this to choose context type)
    RendererAPI::SetAPI(api);

    // Create window (this initializes the graphics context: OpenGL/Vulkan)
    m_Window = std::make_unique<Window>();

    // Now that the context is live, initialize the renderer API (GL state, etc.)
    RenderCommand::Init();
    VE_ENGINE_INFO("Renderer initialized with {0}",
        api == RendererAPI::API::OpenGL ? "OpenGL" : "Vulkan");
}

Application::~Application() {
    Renderer::Shutdown();
    VE_ENGINE_INFO("VibeEngine shutting down");
}

void Application::Run() {
    VE_ENGINE_INFO("Entering main loop");

    while (m_Running) {
        if (m_Window->ShouldClose()) {
            m_Running = false;
            break;
        }

        Renderer::BeginFrame();

        OnUpdate();
        OnRender();

        Renderer::EndFrame();
        m_Window->OnUpdate();
    }

    VE_ENGINE_INFO("Main loop ended");
}

} // namespace VE
