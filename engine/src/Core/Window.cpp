#include "VibeEngine/Core/Window.h"
#include "VibeEngine/Core/Log.h"

#include <GLFW/glfw3.h>

namespace VE {

static bool s_GLFWInitialized = false;

static void GLFWErrorCallback(int error, const char* description) {
    VE_ENGINE_ERROR("GLFW Error ({0}): {1}", error, description);
}

Window::Window(const WindowProps& props) {
    Init(props);
}

Window::~Window() {
    Shutdown();
}

void Window::Init(const WindowProps& props) {
    m_Data.Title  = props.Title;
    m_Data.Width  = props.Width;
    m_Data.Height = props.Height;

    VE_ENGINE_INFO("Creating window: {0} ({1} x {2})", m_Data.Title, m_Data.Width, m_Data.Height);

    if (!s_GLFWInitialized) {
        int success = glfwInit();
        if (!success) {
            VE_ENGINE_ERROR("Failed to initialize GLFW!");
            return;
        }
        glfwSetErrorCallback(GLFWErrorCallback);
        s_GLFWInitialized = true;
    }

    // No OpenGL context yet — just a plain window
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    m_Window = glfwCreateWindow(
        static_cast<int>(m_Data.Width),
        static_cast<int>(m_Data.Height),
        m_Data.Title.c_str(),
        nullptr, nullptr
    );

    if (!m_Window) {
        VE_ENGINE_ERROR("Failed to create GLFW window!");
        return;
    }

    VE_ENGINE_INFO("Window created successfully");
}

void Window::Shutdown() {
    if (m_Window) {
        glfwDestroyWindow(m_Window);
        m_Window = nullptr;
    }
}

void Window::OnUpdate() {
    glfwPollEvents();
}

bool Window::ShouldClose() const {
    return glfwWindowShouldClose(m_Window);
}

} // namespace VE
