#include "VibeEngine/Scene/MeshLibrary.h"
#include "VibeEngine/Renderer/Buffer.h"
#include "VibeEngine/Core/Log.h"

namespace VE {

std::shared_ptr<VertexArray> MeshLibrary::s_Triangle;
std::shared_ptr<VertexArray> MeshLibrary::s_Quad;
std::shared_ptr<VertexArray> MeshLibrary::s_Cube;
std::shared_ptr<Shader>      MeshLibrary::s_DefaultShader;
std::shared_ptr<Shader>      MeshLibrary::s_LitShader;

// ── Unlit shader (2D) ──────────────────────────────────────────────

static const char* s_DefaultVertexSrc = R"(
#version 460 core
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Color;

uniform mat4 u_MVP;

out vec3 v_Color;

void main() {
    v_Color = a_Color;
    gl_Position = u_MVP * vec4(a_Position, 1.0);
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

// ── Lit shader (3D) ─────────────────────────────────────────────────

static const char* s_LitVertexSrc = R"(
#version 460 core
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec3 a_Color;
layout(location = 3) in vec2 a_TexCoord;

uniform mat4 u_MVP;
uniform mat4 u_Model;

out vec3 v_Color;
out vec3 v_Normal;
out vec3 v_FragPos;
out vec2 v_TexCoord;

void main() {
    v_Color    = a_Color;
    v_TexCoord = a_TexCoord;
    v_Normal   = normalize(mat3(transpose(inverse(u_Model))) * a_Normal);
    v_FragPos  = vec3(u_Model * vec4(a_Position, 1.0));
    gl_Position = u_MVP * vec4(a_Position, 1.0);
}
)";

static const char* s_LitFragmentSrc = R"(
#version 460 core
in vec3 v_Color;
in vec3 v_Normal;
in vec3 v_FragPos;
in vec2 v_TexCoord;

uniform vec3 u_LightDir;
uniform vec3 u_ViewPos;
uniform vec4 u_EntityColor;
uniform sampler2D u_Texture;
uniform int u_UseTexture;

out vec4 FragColor;

void main() {
    vec3 baseColor = v_Color;
    if (u_UseTexture == 1)
        baseColor = texture(u_Texture, v_TexCoord).rgb;

    vec3 ambient = 0.15 * baseColor;
    float diff   = max(dot(v_Normal, u_LightDir), 0.0);
    vec3 diffuse = diff * baseColor;

    vec3 viewDir    = normalize(u_ViewPos - v_FragPos);
    vec3 halfwayDir = normalize(u_LightDir + viewDir);
    float spec      = pow(max(dot(v_Normal, halfwayDir), 0.0), 32.0);
    vec3 specular   = vec3(0.3) * spec;

    vec3 result = (ambient + diffuse + specular) * u_EntityColor.rgb;
    FragColor = vec4(result, u_EntityColor.a);
}
)";

void MeshLibrary::Init() {
    // ── Triangle (2D, unlit) ────────────────────────────────────────
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

    // ── Quad (2D, unlit) ────────────────────────────────────────────
    {
        float vertices[] = {
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

    // ── Cube (3D, lit) ──────────────────────────────────────────────
    {
        // 24 vertices: 4 per face, each with position + normal + color + uv
        float vertices[] = {
            // Front face (z = +0.5) — normal (0,0,1)
            -0.5f,-0.5f, 0.5f,  0, 0, 1,  0.8f,0.8f,0.8f,  0,0,
             0.5f,-0.5f, 0.5f,  0, 0, 1,  0.8f,0.8f,0.8f,  1,0,
             0.5f, 0.5f, 0.5f,  0, 0, 1,  0.8f,0.8f,0.8f,  1,1,
            -0.5f, 0.5f, 0.5f,  0, 0, 1,  0.8f,0.8f,0.8f,  0,1,
            // Back face (z = -0.5) — normal (0,0,-1)
            -0.5f,-0.5f,-0.5f,  0, 0,-1,  0.7f,0.7f,0.7f,  1,0,
             0.5f,-0.5f,-0.5f,  0, 0,-1,  0.7f,0.7f,0.7f,  0,0,
             0.5f, 0.5f,-0.5f,  0, 0,-1,  0.7f,0.7f,0.7f,  0,1,
            -0.5f, 0.5f,-0.5f,  0, 0,-1,  0.7f,0.7f,0.7f,  1,1,
            // Top face (y = +0.5) — normal (0,1,0)
            -0.5f, 0.5f,-0.5f,  0, 1, 0,  0.9f,0.9f,0.9f,  0,0,
             0.5f, 0.5f,-0.5f,  0, 1, 0,  0.9f,0.9f,0.9f,  1,0,
             0.5f, 0.5f, 0.5f,  0, 1, 0,  0.9f,0.9f,0.9f,  1,1,
            -0.5f, 0.5f, 0.5f,  0, 1, 0,  0.9f,0.9f,0.9f,  0,1,
            // Bottom face (y = -0.5) — normal (0,-1,0)
            -0.5f,-0.5f,-0.5f,  0,-1, 0,  0.6f,0.6f,0.6f,  0,0,
             0.5f,-0.5f,-0.5f,  0,-1, 0,  0.6f,0.6f,0.6f,  1,0,
             0.5f,-0.5f, 0.5f,  0,-1, 0,  0.6f,0.6f,0.6f,  1,1,
            -0.5f,-0.5f, 0.5f,  0,-1, 0,  0.6f,0.6f,0.6f,  0,1,
            // Right face (x = +0.5) — normal (1,0,0)
             0.5f,-0.5f,-0.5f,  1, 0, 0,  0.75f,0.75f,0.75f,  0,0,
             0.5f, 0.5f,-0.5f,  1, 0, 0,  0.75f,0.75f,0.75f,  0,1,
             0.5f, 0.5f, 0.5f,  1, 0, 0,  0.75f,0.75f,0.75f,  1,1,
             0.5f,-0.5f, 0.5f,  1, 0, 0,  0.75f,0.75f,0.75f,  1,0,
            // Left face (x = -0.5) — normal (-1,0,0)
            -0.5f,-0.5f,-0.5f, -1, 0, 0,  0.65f,0.65f,0.65f,  1,0,
            -0.5f, 0.5f,-0.5f, -1, 0, 0,  0.65f,0.65f,0.65f,  1,1,
            -0.5f, 0.5f, 0.5f, -1, 0, 0,  0.65f,0.65f,0.65f,  0,1,
            -0.5f,-0.5f, 0.5f, -1, 0, 0,  0.65f,0.65f,0.65f,  0,0,
        };
        uint32_t indices[] = {
             0, 1, 2,  2, 3, 0,   // front
             4, 6, 5,  6, 4, 7,   // back
             8, 9,10, 10,11, 8,   // top
            12,14,13, 14,12,15,   // bottom
            16,17,18, 18,19,16,   // right
            20,21,22, 22,23,20,   // left
        };

        s_Cube = VertexArray::Create();
        auto vb = VertexBuffer::Create(vertices, sizeof(vertices));
        vb->SetLayout({
            { ShaderDataType::Float3, "a_Position" },
            { ShaderDataType::Float3, "a_Normal"   },
            { ShaderDataType::Float3, "a_Color"    },
            { ShaderDataType::Float2, "a_TexCoord" },
        });
        s_Cube->AddVertexBuffer(vb);
        s_Cube->SetIndexBuffer(IndexBuffer::Create(indices, 36));
    }

    // ── Shaders ─────────────────────────────────────────────────────
    s_DefaultShader = Shader::Create(s_DefaultVertexSrc, s_DefaultFragmentSrc);
    s_LitShader     = Shader::Create(s_LitVertexSrc, s_LitFragmentSrc);

    VE_ENGINE_INFO("MeshLibrary initialized (Triangle, Quad, Cube)");
}

void MeshLibrary::Shutdown() {
    s_Triangle.reset();
    s_Quad.reset();
    s_Cube.reset();
    s_DefaultShader.reset();
    s_LitShader.reset();
    VE_ENGINE_INFO("MeshLibrary shutdown");
}

std::shared_ptr<VertexArray> MeshLibrary::GetTriangle() { return s_Triangle; }
std::shared_ptr<VertexArray> MeshLibrary::GetQuad()     { return s_Quad; }
std::shared_ptr<VertexArray> MeshLibrary::GetCube()     { return s_Cube; }
std::shared_ptr<Shader>      MeshLibrary::GetDefaultShader() { return s_DefaultShader; }
std::shared_ptr<Shader>      MeshLibrary::GetLitShader()     { return s_LitShader; }

const char* MeshLibrary::GetMeshName(int index) {
    static const char* names[] = { "Triangle", "Quad", "Cube" };
    if (index >= 0 && index < GetMeshCount()) return names[index];
    return "Unknown";
}

std::shared_ptr<VertexArray> MeshLibrary::GetMeshByIndex(int index) {
    switch (index) {
        case 0: return s_Triangle;
        case 1: return s_Quad;
        case 2: return s_Cube;
        default: return nullptr;
    }
}

bool MeshLibrary::IsLitMesh(int index) {
    return index >= 2; // Cube and future 3D meshes
}

int MeshLibrary::GetMeshCount() { return 3; }

} // namespace VE
