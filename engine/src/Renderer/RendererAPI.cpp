#include "VibeEngine/Renderer/RendererAPI.h"
#include "VibeEngine/Platform/OpenGL/OpenGLRendererAPI.h"
#include "VibeEngine/Platform/Vulkan/VulkanRendererAPI.h"
#include "VibeEngine/Core/Log.h"

namespace VE {

RendererAPI::API RendererAPI::s_API = RendererAPI::API::OpenGL;

std::unique_ptr<RendererAPI> RendererAPI::Create() {
    switch (s_API) {
        case API::OpenGL: return std::make_unique<OpenGLRendererAPI>();
        case API::Vulkan: return std::make_unique<VulkanRendererAPI>();
        default:
            VE_ENGINE_ERROR("Unknown RendererAPI!");
            return nullptr;
    }
}

} // namespace VE
