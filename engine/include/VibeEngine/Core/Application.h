#pragma once

#include <memory>

namespace VE {

class Window;

class Application {
public:
    Application();
    virtual ~Application();

    void Run();

private:
    std::unique_ptr<Window> m_Window;
    bool m_Running = true;
};

} // namespace VE
