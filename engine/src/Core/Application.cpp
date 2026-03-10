#include "VibeEngine/Core/Application.h"
#include "VibeEngine/Core/Window.h"
#include "VibeEngine/Core/Log.h"

namespace VE {

Application::Application() {
    Log::Init();
    VE_ENGINE_INFO("VibeEngine v0.1.0 initializing...");

    m_Window = std::make_unique<Window>();
}

Application::~Application() {
    VE_ENGINE_INFO("VibeEngine shutting down");
}

void Application::Run() {
    VE_ENGINE_INFO("Entering main loop");

    while (m_Running) {
        if (m_Window->ShouldClose()) {
            m_Running = false;
            break;
        }

        // TODO: Update subsystems here

        m_Window->OnUpdate();
    }

    VE_ENGINE_INFO("Main loop ended");
}

} // namespace VE
