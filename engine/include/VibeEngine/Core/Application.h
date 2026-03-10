#pragma once

#include "VibeEngine/Renderer/RendererAPI.h"
#include <memory>

namespace VE {

class Window;

class Application {
public:
    explicit Application(RendererAPI::API api = RendererAPI::API::OpenGL);
    virtual ~Application();

    void Run();

    Window& GetWindow() { return *m_Window; }

protected:
    virtual void OnUpdate() {}
    virtual void OnRender() {}

private:
    std::unique_ptr<Window> m_Window;
    bool m_Running = true;
};

} // namespace VE
