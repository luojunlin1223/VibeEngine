#include "VibeEngine/Renderer/Texture.h"
#include "VibeEngine/Renderer/RendererAPI.h"
#include "VibeEngine/Platform/OpenGL/OpenGLTexture.h"
#include "VibeEngine/Platform/Vulkan/VulkanTexture.h"
#include "VibeEngine/Core/Log.h"

namespace VE {

std::shared_ptr<Texture2D> Texture2D::Create(const std::string& path) {
    switch (RendererAPI::GetAPI()) {
        case RendererAPI::API::OpenGL:
            return std::make_shared<OpenGLTexture2D>(path);
        case RendererAPI::API::Vulkan:
            return std::make_shared<VulkanTexture2D>(path);
        default:
            return nullptr;
    }
}

std::shared_ptr<Texture2D> Texture2D::Create(uint32_t width, uint32_t height, const void* data) {
    switch (RendererAPI::GetAPI()) {
        case RendererAPI::API::OpenGL:
            return std::make_shared<OpenGLTexture2D>(width, height, data);
        default:
            return nullptr;
    }
}

} // namespace VE
