#include "VibeEngine/Scene/MeshLibrary.h"
#include "VibeEngine/Renderer/Buffer.h"
#include "VibeEngine/Core/Log.h"

namespace VE {

std::shared_ptr<VertexArray> MeshLibrary::s_Triangle;
std::shared_ptr<VertexArray> MeshLibrary::s_Quad;
std::shared_ptr<Shader>      MeshLibrary::s_DefaultShader;

static const char* s_DefaultVertexSrc = R"(
#version 460 core
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Color;

out vec3 v_Color;

void main() {
    v_Color = a_Color;
    gl_Position = vec4(a_Position, 1.0);
}
)";

static const char* s_DefaultFragmentSrc = R"(
#version 460 core
in vec3 v_Color;
out vec4 FragColor;

void main() {
    FragColor = vec4(v_Color, 1.0);
}
)";

void MeshLibrary::Init() {
    // Triangle
    {
        float vertices[] = {
            // position            // color
            -0.5f, -0.5f, 0.0f,   1.0f, 0.0f, 0.0f,
             0.5f, -0.5f, 0.0f,   0.0f, 1.0f, 0.0f,
             0.0f,  0.5f, 0.0f,   0.0f, 0.0f, 1.0f,
        };
        uint32_t indices[] = { 0, 1, 2 };

        s_Triangle = VertexArray::Create();
        auto vb = VertexBuffer::Create(vertices, sizeof(vertices));
        vb->SetLayout({
            { ShaderDataType::Float3, "a_Position" },
            { ShaderDataType::Float3, "a_Color"    },
        });
        s_Triangle->AddVertexBuffer(vb);
        s_Triangle->SetIndexBuffer(IndexBuffer::Create(indices, 3));
    }

    // Quad
    {
        float vertices[] = {
            // position            // color
            -0.5f, -0.5f, 0.0f,   0.8f, 0.2f, 0.2f,
             0.5f, -0.5f, 0.0f,   0.2f, 0.8f, 0.2f,
             0.5f,  0.5f, 0.0f,   0.2f, 0.2f, 0.8f,
            -0.5f,  0.5f, 0.0f,   0.8f, 0.8f, 0.2f,
        };
        uint32_t indices[] = { 0, 1, 2, 2, 3, 0 };

        s_Quad = VertexArray::Create();
        auto vb = VertexBuffer::Create(vertices, sizeof(vertices));
        vb->SetLayout({
            { ShaderDataType::Float3, "a_Position" },
            { ShaderDataType::Float3, "a_Color"    },
        });
        s_Quad->AddVertexBuffer(vb);
        s_Quad->SetIndexBuffer(IndexBuffer::Create(indices, 6));
    }

    // Default shader
    s_DefaultShader = Shader::Create(s_DefaultVertexSrc, s_DefaultFragmentSrc);

    VE_ENGINE_INFO("MeshLibrary initialized (Triangle, Quad)");
}

void MeshLibrary::Shutdown() {
    s_Triangle.reset();
    s_Quad.reset();
    s_DefaultShader.reset();
    VE_ENGINE_INFO("MeshLibrary shutdown");
}

std::shared_ptr<VertexArray> MeshLibrary::GetTriangle() { return s_Triangle; }
std::shared_ptr<VertexArray> MeshLibrary::GetQuad()     { return s_Quad; }
std::shared_ptr<Shader>      MeshLibrary::GetDefaultShader() { return s_DefaultShader; }

const char* MeshLibrary::GetMeshName(int index) {
    static const char* names[] = { "Triangle", "Quad" };
    if (index >= 0 && index < GetMeshCount()) return names[index];
    return "Unknown";
}

std::shared_ptr<VertexArray> MeshLibrary::GetMeshByIndex(int index) {
    switch (index) {
        case 0: return s_Triangle;
        case 1: return s_Quad;
        default: return nullptr;
    }
}

int MeshLibrary::GetMeshCount() { return 2; }

} // namespace VE
