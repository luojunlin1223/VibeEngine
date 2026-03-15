#include "VibeEngine/Platform/OpenGL/OpenGLVertexArray.h"
#include "VibeEngine/Renderer/GPUResourceTracker.h"

#include <glad/gl.h>

namespace VE {

static GLenum ShaderDataTypeToOpenGLBaseType(ShaderDataType type) {
    switch (type) {
        case ShaderDataType::Float:
        case ShaderDataType::Float2:
        case ShaderDataType::Float3:
        case ShaderDataType::Float4:
        case ShaderDataType::Mat3:
        case ShaderDataType::Mat4:   return GL_FLOAT;
        case ShaderDataType::Int:
        case ShaderDataType::Int2:
        case ShaderDataType::Int3:
        case ShaderDataType::Int4:   return GL_INT;
        case ShaderDataType::Bool:   return GL_BOOL;
        default:                     return 0;
    }
}

OpenGLVertexArray::OpenGLVertexArray() {
    glGenVertexArrays(1, &m_RendererID);
    VE_GPU_TRACK(GPUResourceType::VertexArray, m_RendererID);
}

OpenGLVertexArray::~OpenGLVertexArray() {
    VE_GPU_UNTRACK(GPUResourceType::VertexArray, m_RendererID);
    glDeleteVertexArrays(1, &m_RendererID);
}

void OpenGLVertexArray::Bind() const {
    glBindVertexArray(m_RendererID);
}

void OpenGLVertexArray::Unbind() const {
    glBindVertexArray(0);
}

void OpenGLVertexArray::AddVertexBuffer(const std::shared_ptr<VertexBuffer>& vertexBuffer) {
    glBindVertexArray(m_RendererID);
    vertexBuffer->Bind();

    const auto& layout = vertexBuffer->GetLayout();
    for (const auto& element : layout) {
        glEnableVertexAttribArray(m_VertexBufferIndex);
        glVertexAttribPointer(
            m_VertexBufferIndex,
            static_cast<GLint>(element.GetComponentCount()),
            ShaderDataTypeToOpenGLBaseType(element.Type),
            element.Normalized ? GL_TRUE : GL_FALSE,
            static_cast<GLsizei>(layout.GetStride()),
            reinterpret_cast<const void*>(static_cast<uintptr_t>(element.Offset))
        );
        m_VertexBufferIndex++;
    }

    m_VertexBuffers.push_back(vertexBuffer);
}

void OpenGLVertexArray::AddInstanceBuffer(const std::shared_ptr<VertexBuffer>& instanceBuffer) {
    glBindVertexArray(m_RendererID);
    instanceBuffer->Bind();

    const auto& layout = instanceBuffer->GetLayout();
    for (const auto& element : layout) {
        if (element.Type == ShaderDataType::Mat4) {
            // Mat4 occupies 4 consecutive vec4 attribute slots
            uint32_t floatSize = sizeof(float);
            for (int i = 0; i < 4; i++) {
                glEnableVertexAttribArray(m_VertexBufferIndex);
                glVertexAttribPointer(
                    m_VertexBufferIndex,
                    4,
                    GL_FLOAT,
                    GL_FALSE,
                    static_cast<GLsizei>(layout.GetStride()),
                    reinterpret_cast<const void*>(static_cast<uintptr_t>(element.Offset + floatSize * 4 * i))
                );
                glVertexAttribDivisor(m_VertexBufferIndex, 1);
                m_VertexBufferIndex++;
            }
        } else {
            glEnableVertexAttribArray(m_VertexBufferIndex);
            glVertexAttribPointer(
                m_VertexBufferIndex,
                static_cast<GLint>(element.GetComponentCount()),
                ShaderDataTypeToOpenGLBaseType(element.Type),
                element.Normalized ? GL_TRUE : GL_FALSE,
                static_cast<GLsizei>(layout.GetStride()),
                reinterpret_cast<const void*>(static_cast<uintptr_t>(element.Offset))
            );
            glVertexAttribDivisor(m_VertexBufferIndex, 1);
            m_VertexBufferIndex++;
        }
    }

    m_VertexBuffers.push_back(instanceBuffer);
}

void OpenGLVertexArray::SetIndexBuffer(const std::shared_ptr<IndexBuffer>& indexBuffer) {
    glBindVertexArray(m_RendererID);
    indexBuffer->Bind();
    m_IndexBuffer = indexBuffer;
}

} // namespace VE
