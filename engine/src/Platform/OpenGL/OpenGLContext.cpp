#include "VibeEngine/Platform/OpenGL/OpenGLContext.h"
#include "VibeEngine/Core/Log.h"

#include <glad/gl.h>
#include <GLFW/glfw3.h>

namespace VE {

#ifndef NDEBUG
static void GLAPIENTRY OpenGLDebugCallback(GLenum source, GLenum type, GLuint id,
    GLenum severity, GLsizei length, const GLchar* message, const void* userParam)
{
    (void)source; (void)type; (void)id; (void)length; (void)userParam;

    switch (severity) {
        case GL_DEBUG_SEVERITY_HIGH:   VE_ENGINE_ERROR("[OpenGL] {}", message); break;
        case GL_DEBUG_SEVERITY_MEDIUM: VE_ENGINE_WARN("[OpenGL] {}", message);  break;
        case GL_DEBUG_SEVERITY_LOW:    VE_ENGINE_INFO("[OpenGL] {}", message);  break;
        default: break;
    }
}
#endif

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

#ifndef NDEBUG
    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    glDebugMessageCallback(OpenGLDebugCallback, nullptr);
    // Filter out notification-level messages to reduce noise
    glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_NOTIFICATION, 0, nullptr, GL_FALSE);
    glDebugMessageControl(GL_DONT_CARE, GL_DEBUG_TYPE_PERFORMANCE, GL_DONT_CARE, 0, nullptr, GL_FALSE);
    VE_ENGINE_INFO("OpenGL debug output enabled");
#endif
}

void OpenGLContext::SwapBuffers() {
    glfwSwapBuffers(m_WindowHandle);
}

} // namespace VE
