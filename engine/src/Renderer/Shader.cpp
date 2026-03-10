#include "VibeEngine/Renderer/Shader.h"
#include "VibeEngine/Renderer/RendererAPI.h"
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

} // namespace VE
