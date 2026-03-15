#include "VibeEngine/Renderer/Shader.h"
#include "VibeEngine/Renderer/RendererAPI.h"
#include "VibeEngine/Renderer/ShaderLab.h"
#include "VibeEngine/Renderer/ShaderSources.h"
#include "VibeEngine/Platform/OpenGL/OpenGLShader.h"
#include "VibeEngine/Platform/Vulkan/VulkanShader.h"
#include "VibeEngine/Core/Log.h"
#include <glad/gl.h>

namespace VE {

// ── Render state helpers ────────────────────────────────────────────

static GLenum BlendFactorToGL(BlendFactor f) {
    switch (f) {
        case BlendFactor::One:                 return GL_ONE;
        case BlendFactor::Zero:                return GL_ZERO;
        case BlendFactor::SrcColor:            return GL_SRC_COLOR;
        case BlendFactor::SrcAlpha:            return GL_SRC_ALPHA;
        case BlendFactor::DstColor:            return GL_DST_COLOR;
        case BlendFactor::DstAlpha:            return GL_DST_ALPHA;
        case BlendFactor::OneMinusSrcColor:    return GL_ONE_MINUS_SRC_COLOR;
        case BlendFactor::OneMinusSrcAlpha:    return GL_ONE_MINUS_SRC_ALPHA;
        case BlendFactor::OneMinusDstColor:    return GL_ONE_MINUS_DST_COLOR;
        case BlendFactor::OneMinusDstAlpha:    return GL_ONE_MINUS_DST_ALPHA;
    }
    return GL_ONE;
}

void Shader::ApplyRenderState() const {
    // Blend
    if (m_RenderState.BlendEnabled) {
        glEnable(GL_BLEND);
        glBlendFunc(BlendFactorToGL(m_RenderState.BlendSrc),
                    BlendFactorToGL(m_RenderState.BlendDst));
    } else {
        // Restore default: keep blending enabled with standard alpha blend
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    // Depth write
    glDepthMask(m_RenderState.ZWrite == ZWriteMode::On ? GL_TRUE : GL_FALSE);

    // Cull mode
    if (m_RenderState.Cull == CullMode::Off) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
        glCullFace(m_RenderState.Cull == CullMode::Front ? GL_FRONT : GL_BACK);
    }
}

std::shared_ptr<Shader> Shader::Create(const std::string& vertexSrc, const std::string& fragmentSrc) {
    switch (RendererAPI::GetAPI()) {
        case RendererAPI::API::OpenGL: return std::make_shared<OpenGLShader>(vertexSrc, fragmentSrc);
        case RendererAPI::API::Vulkan: return std::make_shared<VulkanShader>(vertexSrc, fragmentSrc);
        default:
            VE_ENGINE_ERROR("Shader::Create - unsupported API");
            return nullptr;
    }
}

std::shared_ptr<Shader> Shader::CreateFromFile(const std::string& filePath) {
    auto shader = ShaderLabCompiler::CompileFile(filePath);
    if (!shader) {
        VE_ENGINE_WARN("Shader::CreateFromFile('{}') failed — returning magenta error shader", filePath);
        shader = Shader::Create(ErrorVertexSrc, ErrorFragmentSrc);
        if (shader)
            shader->SetName("Error");
    }
    return shader;
}

// ── ShaderLibrary ───────────────────────────────────────────────────

std::unordered_map<std::string, std::shared_ptr<Shader>> ShaderLibrary::s_Shaders;

void ShaderLibrary::Register(const std::string& name, const std::shared_ptr<Shader>& shader) {
    shader->SetName(name);
    s_Shaders[name] = shader;
}

void ShaderLibrary::Remove(const std::string& name) {
    s_Shaders.erase(name);
}

std::shared_ptr<Shader> ShaderLibrary::Get(const std::string& name) {
    auto it = s_Shaders.find(name);
    if (it != s_Shaders.end()) return it->second;
    return nullptr;
}

bool ShaderLibrary::Exists(const std::string& name) {
    return s_Shaders.find(name) != s_Shaders.end();
}

std::vector<std::string> ShaderLibrary::GetAllNames() {
    std::vector<std::string> names;
    names.reserve(s_Shaders.size());
    for (auto& [name, _] : s_Shaders)
        names.push_back(name);
    return names;
}

void ShaderLibrary::Shutdown() {
    s_Shaders.clear();
}

} // namespace VE
