#pragma once

#include <string>
#include <cstdint>

struct GLFWwindow;

namespace VE {

struct WindowProps {
    std::string Title;
    uint32_t    Width;
    uint32_t    Height;

    WindowProps(const std::string& title = "VibeEngine",
                uint32_t width = 1280,
                uint32_t height = 720)
        : Title(title), Width(width), Height(height) {}
};

class Window {
public:
    Window(const WindowProps& props = WindowProps());
    ~Window();

    void OnUpdate();
    bool ShouldClose() const;

    uint32_t GetWidth()  const { return m_Data.Width; }
    uint32_t GetHeight() const { return m_Data.Height; }

    GLFWwindow* GetNativeWindow() const { return m_Window; }

private:
    void Init(const WindowProps& props);
    void Shutdown();

    GLFWwindow* m_Window = nullptr;

    struct WindowData {
        std::string Title;
        uint32_t    Width  = 0;
        uint32_t    Height = 0;
    };

    WindowData m_Data;
};

} // namespace VE
