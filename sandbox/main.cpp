#include <VibeEngine/VibeEngine.h>

class Sandbox : public VE::Application {
public:
    Sandbox() {
        VE_INFO("Sandbox application created");
    }

    ~Sandbox() override {
        VE_INFO("Sandbox application destroyed");
    }
};

int main() {
    Sandbox app;
    app.Run();
    return 0;
}
