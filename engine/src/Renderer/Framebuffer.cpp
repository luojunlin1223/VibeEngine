#include "VibeEngine/Renderer/Framebuffer.h"
#include "VibeEngine/Renderer/RendererAPI.h"
#include "VibeEngine/Platform/OpenGL/OpenGLFramebuffer.h"
#include "VibeEngine/Core/Log.h"

namespace VE {

std::shared_ptr<Framebuffer> Framebuffer::Create(const FramebufferSpec& spec) {
    switch (RendererAPI::GetAPI()) {
        case RendererAPI::API::OpenGL:
            return std::make_shared<OpenGLFramebuffer>(spec);
        case RendererAPI::API::Vulkan:
            VE_ENGINE_WARN("Framebuffer not yet supported on Vulkan, rendering to screen");
            return nullptr;
        default:
            return nullptr;
    }
}

} // namespace VE
