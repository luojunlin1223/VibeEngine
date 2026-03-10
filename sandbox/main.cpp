#include <VibeEngine/VibeEngine.h>

// ── Basic shaders for the triangle demo ─────────────────────────────
static const char* vertexShaderSrc = R"(
#version 460 core
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Color;

out vec3 v_Color;

void main() {
    v_Color = a_Color;
    gl_Position = vec4(a_Position, 1.0);
}
)";

static const char* fragmentShaderSrc = R"(
#version 460 core
in vec3 v_Color;
out vec4 FragColor;

void main() {
    FragColor = vec4(v_Color, 1.0);
}
)";

class Sandbox : public VE::Application {
public:
    Sandbox()
        : VE::Application(VE::RendererAPI::API::OpenGL)
    {
        VE_INFO("Sandbox application created");

        // Only set up OpenGL rendering objects when using OpenGL
        if (VE::Renderer::GetAPI() != VE::RendererAPI::API::OpenGL)
            return;

        // Triangle vertices: position (x,y,z) + color (r,g,b)
        float vertices[] = {
            // positions          // colors
            -0.5f, -0.5f, 0.0f,  1.0f, 0.0f, 0.0f,  // bottom-left  (red)
             0.5f, -0.5f, 0.0f,  0.0f, 1.0f, 0.0f,  // bottom-right (green)
             0.0f,  0.5f, 0.0f,  0.0f, 0.0f, 1.0f,  // top          (blue)
        };

        uint32_t indices[] = { 0, 1, 2 };

        m_VertexArray = VE::VertexArray::Create();

        auto vertexBuffer = VE::VertexBuffer::Create(vertices, sizeof(vertices));
        vertexBuffer->SetLayout({
            { VE::ShaderDataType::Float3, "a_Position" },
            { VE::ShaderDataType::Float3, "a_Color"    },
        });
        m_VertexArray->AddVertexBuffer(vertexBuffer);

        auto indexBuffer = VE::IndexBuffer::Create(indices, 3);
        m_VertexArray->SetIndexBuffer(indexBuffer);

        m_Shader = VE::Shader::Create(vertexShaderSrc, fragmentShaderSrc);
    }

    ~Sandbox() override {
        VE_INFO("Sandbox application destroyed");
    }

protected:
    void OnRender() override {
        if (!m_Shader) return;

        m_Shader->Bind();
        VE::RenderCommand::DrawIndexed(m_VertexArray);
    }

private:
    std::shared_ptr<VE::VertexArray> m_VertexArray;
    std::shared_ptr<VE::Shader>     m_Shader;
};

int main() {
    Sandbox app;
    app.Run();
    return 0;
}
