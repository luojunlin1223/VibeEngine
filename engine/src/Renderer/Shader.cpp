#include "VibeEngine/Renderer/Shader.h"
#include "VibeEngine/Renderer/RendererAPI.h"
#include "VibeEngine/Renderer/ShaderLab.h"
#include "VibeEngine/Platform/OpenGL/OpenGLShader.h"
#include "VibeEngine/Platform/Vulkan/VulkanShader.h"
#include "VibeEngine/Core/Log.h"

namespace VE {

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
    return ShaderLabCompiler::CompileFile(filePath);
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
