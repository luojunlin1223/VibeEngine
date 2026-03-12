#pragma once

#include "VibeEngine/Renderer/RendererAPI.h"
#include <memory>

namespace VE {

class Window;
class ImGuiLayer;

class Application {
public:
    explicit Application(RendererAPI::API api = RendererAPI::API::OpenGL);
    virtual ~Application();

    void Run();

    Window& GetWindow() { return *m_Window; }
    RendererAPI::API GetCurrentAPI() const { return m_CurrentAPI; }
    float GetDeltaTime() const { return m_DeltaTime; }

    static Application* GetInstance() { return s_Instance; }

    // Request a backend switch — takes effect at the end of the current frame
    void RequestSwitchAPI(RendererAPI::API newAPI);

protected:
    virtual void OnUpdate() {}
    virtual void OnRender() {}
    virtual void OnImGuiRender() {}

    // Called after the renderer backend has been switched, so the
    // application can recreate its GPU resources (meshes, shaders, etc.)
    virtual void OnRendererReloaded() {}

private:
    void SwitchAPI(RendererAPI::API newAPI);

    std::unique_ptr<Window> m_Window;
    std::unique_ptr<ImGuiLayer> m_ImGuiLayer;
    RendererAPI::API m_CurrentAPI = RendererAPI::API::OpenGL;
    RendererAPI::API m_PendingAPI = RendererAPI::API::None; // None = no switch pending
    bool m_Running = true;
    float m_LastFrameTime = 0.0f;

    static Application* s_Instance;

protected:
    float m_DeltaTime = 0.0f;
};

} // namespace VE
