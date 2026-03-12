#pragma once

#include "VibeEngine/Renderer/GraphicsContext.h"

struct GLFWwindow;

namespace VE {

class OpenGLContext : public GraphicsContext {
public:
    explicit OpenGLContext(GLFWwindow* windowHandle);

    void Init() override;
    void SwapBuffers() override;

private:
    GLFWwindow* m_WindowHandle;
};

} // namespace VE
