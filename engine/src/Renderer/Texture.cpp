#include "VibeEngine/Renderer/Texture.h"
#include "VibeEngine/Renderer/RendererAPI.h"
#include "VibeEngine/Platform/OpenGL/OpenGLTexture.h"
#include "VibeEngine/Core/Log.h"

namespace VE {

std::shared_ptr<Texture2D> Texture2D::Create(const std::string& path) {
    switch (RendererAPI::GetAPI()) {
        case RendererAPI::API::OpenGL:
            return std::make_shared<OpenGLTexture2D>(path);
        case RendererAPI::API::Vulkan:
            VE_ENGINE_WARN("Texture loading not yet supported on Vulkan");
            return nullptr;
        default:
            return nullptr;
    }
}

} // namespace VE
