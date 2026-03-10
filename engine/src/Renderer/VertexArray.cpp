#include "VibeEngine/Renderer/VertexArray.h"
#include "VibeEngine/Renderer/RendererAPI.h"
#include "VibeEngine/Platform/OpenGL/OpenGLVertexArray.h"
#include "VibeEngine/Core/Log.h"

namespace VE {

std::shared_ptr<VertexArray> VertexArray::Create() {
    switch (RendererAPI::GetAPI()) {
        case RendererAPI::API::OpenGL: return std::make_shared<OpenGLVertexArray>();
        default:
            VE_ENGINE_ERROR("VertexArray::Create - unsupported API");
            return nullptr;
    }
}

} // namespace VE
