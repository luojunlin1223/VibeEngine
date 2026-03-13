#include "VibeEngine/Scene/MeshLibrary.h"
#include "VibeEngine/Renderer/Buffer.h"
#include "VibeEngine/Renderer/ShaderLab.h"
#include "VibeEngine/Core/Log.h"
#include <glm/glm.hpp>

#include <cmath>
#include <vector>
#include <filesystem>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace VE {

std::shared_ptr<VertexArray> MeshLibrary::s_Triangle;
std::shared_ptr<VertexArray> MeshLibrary::s_Quad;
std::shared_ptr<VertexArray> MeshLibrary::s_Cube;
std::shared_ptr<VertexArray> MeshLibrary::s_Sphere;
std::shared_ptr<Shader>      MeshLibrary::s_DefaultShader;
std::shared_ptr<Shader>      MeshLibrary::s_LitShader;
std::shared_ptr<Shader>      MeshLibrary::s_SkyShader;

// ── Unlit shader (2D) ──────────────────────────────────────────────

static const char* s_DefaultVertexSrc = R"(
#version 460 core
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec3 a_Color;
layout(location = 3) in vec2 a_TexCoord;

uniform mat4 u_MVP;

out vec3 v_Color;
out vec2 v_TexCoord;

void main() {
    v_Color    = a_Color;
    v_TexCoord = a_TexCoord;
    gl_Position = u_MVP * vec4(a_Position, 1.0);
}
)";

static const char* s_DefaultFragmentSrc = R"(
#version 460 core
in vec3 v_Color;
in vec2 v_TexCoord;

uniform sampler2D u_Texture;
uniform int u_UseTexture;
uniform vec4 u_EntityColor;

out vec4 FragColor;

void main() {
    vec3 baseColor = v_Color;
    if (u_UseTexture == 1)
        baseColor = texture(u_Texture, v_TexCoord).rgb;
    baseColor *= u_EntityColor.rgb;
    FragColor = vec4(baseColor, u_EntityColor.a);
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

uniform vec3  u_LightDir;
uniform vec3  u_LightColor;
uniform float u_LightIntensity;
uniform vec3  u_ViewPos;
uniform vec4  u_EntityColor;
uniform float u_Metallic;
uniform float u_Roughness;
uniform float u_AO;
uniform sampler2D u_Texture;
uniform int   u_UseTexture;

out vec4 FragColor;

const float PI = 3.14159265359;

float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float NdotH  = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 0.0000001);
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    return GeometrySchlickGGX(max(dot(N, V), 0.0), roughness)
         * GeometrySchlickGGX(max(dot(N, L), 0.0), roughness);
}

vec3 FresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

void main() {
    vec3 albedo = v_Color;
    if (u_UseTexture == 1)
        albedo = texture(u_Texture, v_TexCoord).rgb;
    albedo *= u_EntityColor.rgb;

    float metallic  = u_Metallic;
    float roughness = max(u_Roughness, 0.04);
    float ao        = u_AO;

    vec3 N = normalize(v_Normal);
    vec3 V = normalize(u_ViewPos - v_FragPos);

    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    vec3 L = normalize(u_LightDir);
    vec3 H = normalize(V + L);
    vec3 radiance = u_LightColor * u_LightIntensity;

    float NDF = DistributionGGX(N, H, roughness);
    float G   = GeometrySmith(N, V, L, roughness);
    vec3  F   = FresnelSchlick(max(dot(H, V), 0.0), F0);

    vec3  specular = (NDF * G * F) / max(4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0), 0.001);
    vec3  kD = (vec3(1.0) - F) * (1.0 - metallic);

    float NdotL = max(dot(N, L), 0.0);
    vec3 Lo = (kD * albedo / PI + specular) * radiance * NdotL;

    vec3 ambient = vec3(0.03) * albedo * ao;
    vec3 color = ambient + Lo;

    // Tone mapping + gamma
    color = color / (color + vec3(1.0));
    color = pow(color, vec3(1.0 / 2.2));

    FragColor = vec4(color, u_EntityColor.a);
}
)";

// ── Sky shader ──────────────────────────────────────────────────────

static const char* s_SkyVertexSrc = R"(
#version 460 core
layout(location = 0) in vec3 a_Position;

uniform mat4 u_MVP;

out vec3 v_Dir;

void main() {
    v_Dir = a_Position;
    vec4 pos = u_MVP * vec4(a_Position, 1.0);
    gl_Position = pos.xyww; // depth = 1.0 (far plane)
}
)";

static const char* s_SkyFragmentSrc = R"(
#version 460 core
in vec3 v_Dir;

uniform vec3 u_TopColor;
uniform vec3 u_BottomColor;
uniform sampler2D u_Texture;
uniform int u_UseTexture;

out vec4 FragColor;

void main() {
    vec3 dir = normalize(v_Dir);
    if (u_UseTexture == 1) {
        // Equirectangular mapping
        float u = atan(dir.z, dir.x) / (2.0 * 3.14159265) + 0.5;
        float v = asin(clamp(dir.y, -1.0, 1.0)) / 3.14159265 + 0.5;
        FragColor = vec4(texture(u_Texture, vec2(u, v)).rgb, 1.0);
    } else {
        float t = dir.y * 0.5 + 0.5; // 0 = bottom, 1 = top
        vec3 color = mix(u_BottomColor, u_TopColor, t);
        FragColor = vec4(color, 1.0);
    }
}
)";

void MeshLibrary::Init() {
    // ── Triangle ────────────────────────────────────────────────────
    {
        float vertices[] = {
            // position            // normal             // color              // uv
            -0.5f, -0.5f, 0.0f,   0.0f, 0.0f, 1.0f,   1.0f, 0.0f, 0.0f,   0.0f, 0.0f,
             0.5f, -0.5f, 0.0f,   0.0f, 0.0f, 1.0f,   0.0f, 1.0f, 0.0f,   1.0f, 0.0f,
             0.0f,  0.5f, 0.0f,   0.0f, 0.0f, 1.0f,   0.0f, 0.0f, 1.0f,   0.5f, 1.0f,
        };
        uint32_t indices[] = { 0, 1, 2 };

        s_Triangle = VertexArray::Create();
        auto vb = VertexBuffer::Create(vertices, sizeof(vertices));
        vb->SetLayout({
            { ShaderDataType::Float3, "a_Position" },
            { ShaderDataType::Float3, "a_Normal"   },
            { ShaderDataType::Float3, "a_Color"    },
            { ShaderDataType::Float2, "a_TexCoord" },
        });
        s_Triangle->AddVertexBuffer(vb);
        s_Triangle->SetIndexBuffer(IndexBuffer::Create(indices, 3));
    }

    // ── Quad ────────────────────────────────────────────────────────
    {
        float vertices[] = {
            // position            // normal             // color              // uv
            -0.5f, -0.5f, 0.0f,   0.0f, 0.0f, 1.0f,   1.0f, 1.0f, 1.0f,   0.0f, 0.0f,
             0.5f, -0.5f, 0.0f,   0.0f, 0.0f, 1.0f,   1.0f, 1.0f, 1.0f,   1.0f, 0.0f,
             0.5f,  0.5f, 0.0f,   0.0f, 0.0f, 1.0f,   1.0f, 1.0f, 1.0f,   1.0f, 1.0f,
            -0.5f,  0.5f, 0.0f,   0.0f, 0.0f, 1.0f,   1.0f, 1.0f, 1.0f,   0.0f, 1.0f,
        };
        uint32_t indices[] = { 0, 1, 2, 2, 3, 0 };

        s_Quad = VertexArray::Create();
        auto vb = VertexBuffer::Create(vertices, sizeof(vertices));
        vb->SetLayout({
            { ShaderDataType::Float3, "a_Position" },
            { ShaderDataType::Float3, "a_Normal"   },
            { ShaderDataType::Float3, "a_Color"    },
            { ShaderDataType::Float2, "a_TexCoord" },
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

    // ── Sphere (sky, position-only) ────────────────────────────────
    {
        const int rings = 32;
        const int segments = 64;
        std::vector<float> vertices;
        std::vector<uint32_t> indices;

        for (int r = 0; r <= rings; r++) {
            float phi = static_cast<float>(M_PI) * static_cast<float>(r) / static_cast<float>(rings);
            float y   = std::cos(phi);
            float sinPhi = std::sin(phi);

            for (int s = 0; s <= segments; s++) {
                float theta = 2.0f * static_cast<float>(M_PI) * static_cast<float>(s) / static_cast<float>(segments);
                float x = sinPhi * std::cos(theta);
                float z = sinPhi * std::sin(theta);

                vertices.push_back(x);
                vertices.push_back(y);
                vertices.push_back(z);
            }
        }

        for (int r = 0; r < rings; r++) {
            for (int s = 0; s < segments; s++) {
                uint32_t a = static_cast<uint32_t>(r * (segments + 1) + s);
                uint32_t b = a + static_cast<uint32_t>(segments + 1);
                // Reversed winding so inside faces are front-facing
                indices.push_back(a);
                indices.push_back(a + 1);
                indices.push_back(b);
                indices.push_back(b);
                indices.push_back(a + 1);
                indices.push_back(b + 1);
            }
        }

        s_Sphere = VertexArray::Create();
        auto vb = VertexBuffer::Create(vertices.data(),
            static_cast<uint32_t>(vertices.size() * sizeof(float)));
        vb->SetLayout({
            { ShaderDataType::Float3, "a_Position" },
        });
        s_Sphere->AddVertexBuffer(vb);
        s_Sphere->SetIndexBuffer(IndexBuffer::Create(indices.data(),
            static_cast<uint32_t>(indices.size())));
    }

    // ── Shaders (try ShaderLab files first, fallback to hardcoded) ──
    {
        const std::string shaderDir = "shaders/";
        auto tryLoadShader = [&](const std::string& filename,
                                 const char* vertFallback, const char* fragFallback)
            -> std::shared_ptr<Shader>
        {
            std::string path = shaderDir + filename;
            if (std::filesystem::exists(path)) {
                auto shader = Shader::CreateFromFile(path);
                if (shader) {
                    VE_ENGINE_INFO("ShaderLab: Loaded '{}'", path);
                    return shader;
                }
                VE_ENGINE_WARN("ShaderLab: Failed to compile '{}', using fallback", path);
            }
            return Shader::Create(vertFallback, fragFallback);
        };

        s_DefaultShader = tryLoadShader("Unlit.shader", s_DefaultVertexSrc, s_DefaultFragmentSrc);
        s_LitShader     = tryLoadShader("Lit.shader",   s_LitVertexSrc,     s_LitFragmentSrc);
        s_SkyShader     = tryLoadShader("Sky.shader",   s_SkyVertexSrc,     s_SkyFragmentSrc);

        // Register built-in shaders in ShaderLibrary
        ShaderLibrary::Register("Default", s_DefaultShader);
        ShaderLibrary::Register("Lit",     s_LitShader);
        ShaderLibrary::Register("Sky",     s_SkyShader);
    }

    // ── Built-in Materials ──────────────────────────────────────────
    {
        auto defaultMat = Material::Create("Default", s_DefaultShader);
        defaultMat->SetLit(false);
        defaultMat->PopulateFromShader();
        MaterialLibrary::Register(defaultMat);

        auto litMat = Material::Create("Lit", s_LitShader);
        litMat->SetLit(true);
        litMat->PopulateFromShader(); // auto-fill from ShaderLab Properties
        MaterialLibrary::Register(litMat);

        auto skyMat = Material::Create("Sky", s_SkyShader);
        skyMat->SetLit(false);
        skyMat->PopulateFromShader();
        MaterialLibrary::Register(skyMat);
    }

    VE_ENGINE_INFO("MeshLibrary initialized (Triangle, Quad, Cube, Sphere)");
}

void MeshLibrary::Shutdown() {
    MaterialLibrary::Shutdown();
    ShaderLibrary::Shutdown();
    s_Triangle.reset();
    s_Quad.reset();
    s_Cube.reset();
    s_Sphere.reset();
    s_DefaultShader.reset();
    s_LitShader.reset();
    s_SkyShader.reset();
    VE_ENGINE_INFO("MeshLibrary shutdown");
}

std::shared_ptr<VertexArray> MeshLibrary::GetTriangle() { return s_Triangle; }
std::shared_ptr<VertexArray> MeshLibrary::GetQuad()     { return s_Quad; }
std::shared_ptr<VertexArray> MeshLibrary::GetCube()     { return s_Cube; }
std::shared_ptr<VertexArray> MeshLibrary::GetSphere()   { return s_Sphere; }
std::shared_ptr<Shader>      MeshLibrary::GetDefaultShader() { return s_DefaultShader; }
std::shared_ptr<Shader>      MeshLibrary::GetLitShader()     { return s_LitShader; }
std::shared_ptr<Shader>      MeshLibrary::GetSkyShader()     { return s_SkyShader; }

const char* MeshLibrary::GetMeshName(int index) {
    static const char* names[] = { "Triangle", "Quad", "Cube", "Sphere" };
    if (index >= 0 && index < GetMeshCount()) return names[index];
    return "Unknown";
}

std::shared_ptr<VertexArray> MeshLibrary::GetMeshByIndex(int index) {
    switch (index) {
        case 0: return s_Triangle;
        case 1: return s_Quad;
        case 2: return s_Cube;
        case 3: return s_Sphere;
        default: return nullptr;
    }
}

bool MeshLibrary::IsLitMesh(int index) {
    return index >= 2 && index <= 2; // Only Cube uses lit shader
}

int MeshLibrary::GetMeshCount() { return 4; }

AABB MeshLibrary::GetMeshAABB(int index) {
    AABB box;
    switch (index) {
        case 0: // Triangle
        case 1: // Quad
            box.Min = glm::vec3(-0.5f, -0.5f, 0.0f);
            box.Max = glm::vec3( 0.5f,  0.5f, 0.0f);
            break;
        case 2: // Cube
            box.Min = glm::vec3(-0.5f, -0.5f, -0.5f);
            box.Max = glm::vec3( 0.5f,  0.5f,  0.5f);
            break;
        case 3: // Sphere
            box.Min = glm::vec3(-1.0f, -1.0f, -1.0f);
            box.Max = glm::vec3( 1.0f,  1.0f,  1.0f);
            break;
        default:
            box.Min = glm::vec3(-0.5f);
            box.Max = glm::vec3( 0.5f);
            break;
    }
    return box;
}

} // namespace VE
