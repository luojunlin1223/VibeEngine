#include "VibeEngine/Platform/OpenGL/OpenGLContext.h"
#include "VibeEngine/Core/Log.h"

#include <glad/gl.h>
#include <GLFW/glfw3.h>

namespace VE {

OpenGLContext::OpenGLContext(GLFWwindow* windowHandle)
    : m_WindowHandle(windowHandle) {}

void OpenGLContext::Init() {
    glfwMakeContextCurrent(m_WindowHandle);

    int version = gladLoadGL(glfwGetProcAddress);
    if (!version) {
        VE_ENGINE_ERROR("Failed to initialize glad (OpenGL loader)!");
        return;
    }

    VE_ENGINE_INFO("OpenGL Info:");
    VE_ENGINE_INFO("  Vendor:   {0}", reinterpret_cast<const char*>(glGetString(GL_VENDOR)));
    VE_ENGINE_INFO("  Renderer: {0}", reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
    VE_ENGINE_INFO("  Version:  {0}", reinterpret_cast<const char*>(glGetString(GL_VERSION)));
}

void OpenGLContext::SwapBuffers() {
    glfwSwapBuffers(m_WindowHandle);
}

} // namespace VE
