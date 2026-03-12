#include "VibeEngine/Core/Window.h"
#include "VibeEngine/Core/Log.h"
#include "VibeEngine/Renderer/RendererAPI.h"
#include "VibeEngine/Platform/OpenGL/OpenGLContext.h"
#include "VibeEngine/Platform/Vulkan/VulkanContext.h"

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

    // Reset all window hints to defaults before setting new ones.
    // This is critical when switching backends (e.g. OpenGL → Vulkan)
    // to avoid stale hints from the previous window.
    glfwDefaultWindowHints();

    // Configure GLFW window hints based on graphics API
    auto api = RendererAPI::GetAPI();
    if (api == RendererAPI::API::OpenGL) {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    } else {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    }
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    glfwWindowHint(GLFW_FOCUSED, GLFW_TRUE);

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

    // Create graphics context based on API
    if (api == RendererAPI::API::OpenGL) {
        m_Context = std::make_unique<OpenGLContext>(m_Window);
    } else if (api == RendererAPI::API::Vulkan) {
        m_Context = std::make_unique<VulkanContext>(m_Window);
    }

    m_Context->Init();

    VE_ENGINE_INFO("Window created successfully");
}

void Window::Shutdown() {
    m_Context.reset();
    if (m_Window) {
        glfwDestroyWindow(m_Window);
        m_Window = nullptr;
    }
}

void Window::OnUpdate() {
    glfwPollEvents();
    m_Context->SwapBuffers();
}

bool Window::ShouldClose() const {
    return glfwWindowShouldClose(m_Window);
}

} // namespace VE
