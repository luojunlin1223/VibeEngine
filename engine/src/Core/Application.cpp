#include "VibeEngine/Core/Application.h"
#include "VibeEngine/Core/Window.h"
#include "VibeEngine/Core/Log.h"
#include "VibeEngine/Renderer/Renderer.h"
#include "VibeEngine/Renderer/RenderCommand.h"
#include "VibeEngine/ImGui/ImGuiLayer.h"

#include <GLFW/glfw3.h>

namespace VE {

Application* Application::s_Instance = nullptr;

Application::Application(RendererAPI::API api)
    : m_CurrentAPI(api)
{
    s_Instance = this;
    Log::Init();
    VE_ENGINE_INFO("VibeEngine v0.1.0 initializing...");

    RendererAPI::SetAPI(api);

    m_Window = std::make_unique<Window>();

    RenderCommand::Init();
    VE_ENGINE_INFO("Renderer initialized with {0}",
        api == RendererAPI::API::OpenGL ? "OpenGL" : "Vulkan");

    m_ImGuiLayer = std::make_unique<ImGuiLayer>();
    m_ImGuiLayer->Init(m_Window->GetNativeWindow(), api);
}

Application::~Application() {
    if (m_ImGuiLayer) {
        m_ImGuiLayer->Shutdown();
    }
    Renderer::Shutdown();
    VE_ENGINE_INFO("VibeEngine shutting down");
}

void Application::RequestSwitchAPI(RendererAPI::API newAPI) {
    if (newAPI != m_CurrentAPI)
        m_PendingAPI = newAPI;
}

void Application::SwitchAPI(RendererAPI::API newAPI) {
    VE_ENGINE_INFO("Switching renderer from {0} to {1}",
        m_CurrentAPI == RendererAPI::API::OpenGL ? "OpenGL" : "Vulkan",
        newAPI == RendererAPI::API::OpenGL ? "OpenGL" : "Vulkan");

    // 1. Shutdown ImGui (this also destroys any viewport OS windows)
    m_ImGuiLayer->Shutdown();
    m_ImGuiLayer.reset();

    // Flush GLFW events so viewport windows are properly destroyed
    glfwPollEvents();

    // 2. Let the application release GPU resources
    OnRendererReloaded(); // first call: release old resources

    // 3. Shutdown renderer
    RenderCommand::Shutdown();
    Renderer::Shutdown();

    // 4. Destroy old window (and graphics context)
    m_Window.reset();

    // 5. Set new API and recreate everything
    m_CurrentAPI = newAPI;
    RendererAPI::SetAPI(newAPI);

    m_Window = std::make_unique<Window>();

    RenderCommand::Init();
    VE_ENGINE_INFO("Renderer re-initialized with {0}",
        newAPI == RendererAPI::API::OpenGL ? "OpenGL" : "Vulkan");

    // 6. Reinitialize ImGui
    m_ImGuiLayer = std::make_unique<ImGuiLayer>();
    m_ImGuiLayer->Init(m_Window->GetNativeWindow(), newAPI);

    // 7. Let the application recreate GPU resources
    OnRendererReloaded(); // second call: recreate with new backend

    // 8. Make sure the new window is visible and focused
    glfwShowWindow(m_Window->GetNativeWindow());
    glfwFocusWindow(m_Window->GetNativeWindow());

    VE_ENGINE_INFO("Backend switch complete");
}

void Application::Run() {
    VE_ENGINE_INFO("Entering main loop");

    while (m_Running) {
        if (m_Window->ShouldClose()) {
            m_Running = false;
            break;
        }

        Renderer::BeginFrame();

        // Set viewport every frame to the actual framebuffer size.
        // Vulkan does this in RecordCommandBuffer; for OpenGL we must
        // do it explicitly because ImGui (viewports) can change glViewport.
        {
            int fbW = 0, fbH = 0;
            glfwGetFramebufferSize(m_Window->GetNativeWindow(), &fbW, &fbH);
            if (fbW > 0 && fbH > 0)
                RenderCommand::SetViewport(0, 0, static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH));
        }

        float time = static_cast<float>(glfwGetTime());
        m_DeltaTime = time - m_LastFrameTime;
        m_LastFrameTime = time;

        OnUpdate();
        OnRender();

        // ImGui rendering pass
        if (m_ImGuiLayer) {
            m_ImGuiLayer->Begin();
            OnImGuiRender();
            m_ImGuiLayer->End();
        }

        Renderer::EndFrame();
        m_Window->OnUpdate();

        // Handle pending API switch at the end of the frame
        if (m_PendingAPI != RendererAPI::API::None) {
            RendererAPI::API target = m_PendingAPI;
            m_PendingAPI = RendererAPI::API::None;
            SwitchAPI(target);
        }
    }

    VE_ENGINE_INFO("Main loop ended");
}

} // namespace VE
