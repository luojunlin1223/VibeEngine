#include "VibeEngine/Renderer/Buffer.h"
#include "VibeEngine/Renderer/RendererAPI.h"
#include "VibeEngine/Platform/OpenGL/OpenGLBuffer.h"
#include "VibeEngine/Platform/Vulkan/VulkanBuffer.h"
#include "VibeEngine/Core/Log.h"

namespace VE {

// ── ShaderDataType helpers ──────────────────────────────────────────

uint32_t ShaderDataTypeSize(ShaderDataType type) {
    switch (type) {
        case ShaderDataType::Float:  return 4;
        case ShaderDataType::Float2: return 4 * 2;
        case ShaderDataType::Float3: return 4 * 3;
        case ShaderDataType::Float4: return 4 * 4;
        case ShaderDataType::Int:    return 4;
        case ShaderDataType::Int2:   return 4 * 2;
        case ShaderDataType::Int3:   return 4 * 3;
        case ShaderDataType::Int4:   return 4 * 4;
        case ShaderDataType::Mat3:   return 4 * 3 * 3;
        case ShaderDataType::Mat4:   return 4 * 4 * 4;
        case ShaderDataType::Bool:   return 1;
        default: return 0;
    }
}

BufferElement::BufferElement(ShaderDataType type, const std::string& name, bool normalized)
    : Name(name), Type(type), Size(ShaderDataTypeSize(type)), Offset(0), Normalized(normalized) {}

uint32_t BufferElement::GetComponentCount() const {
    switch (Type) {
        case ShaderDataType::Float:  return 1;
        case ShaderDataType::Float2: return 2;
        case ShaderDataType::Float3: return 3;
        case ShaderDataType::Float4: return 4;
        case ShaderDataType::Int:    return 1;
        case ShaderDataType::Int2:   return 2;
        case ShaderDataType::Int3:   return 3;
        case ShaderDataType::Int4:   return 4;
        case ShaderDataType::Mat3:   return 3 * 3;
        case ShaderDataType::Mat4:   return 4 * 4;
        case ShaderDataType::Bool:   return 1;
        default: return 0;
    }
}

BufferLayout::BufferLayout(std::initializer_list<BufferElement> elements)
    : m_Elements(elements) {
    CalculateOffsetsAndStride();
}

void BufferLayout::CalculateOffsetsAndStride() {
    uint32_t offset = 0;
    m_Stride = 0;
    for (auto& element : m_Elements) {
        element.Offset = offset;
        offset += element.Size;
        m_Stride += element.Size;
    }
}

// ── Factory methods ─────────────────────────────────────────────────

std::shared_ptr<VertexBuffer> VertexBuffer::Create(const float* vertices, uint32_t size) {
    switch (RendererAPI::GetAPI()) {
        case RendererAPI::API::OpenGL: return std::make_shared<OpenGLVertexBuffer>(vertices, size);
        case RendererAPI::API::Vulkan: return std::make_shared<VulkanVertexBuffer>(vertices, size);
        default:
            VE_ENGINE_ERROR("VertexBuffer::Create - unsupported API");
            return nullptr;
    }
}

std::shared_ptr<IndexBuffer> IndexBuffer::Create(const uint32_t* indices, uint32_t count) {
    switch (RendererAPI::GetAPI()) {
        case RendererAPI::API::OpenGL: return std::make_shared<OpenGLIndexBuffer>(indices, count);
        case RendererAPI::API::Vulkan: return std::make_shared<VulkanIndexBuffer>(indices, count);
        default:
            VE_ENGINE_ERROR("IndexBuffer::Create - unsupported API");
            return nullptr;
    }
}

} // namespace VE
