#include "VibeEngine/Platform/OpenGL/OpenGLBuffer.h"
#include "VibeEngine/Renderer/GPUResourceTracker.h"

#include <glad/gl.h>

namespace VE {

// ── VertexBuffer ────────────────────────────────────────────────────

OpenGLVertexBuffer::OpenGLVertexBuffer(const float* vertices, uint32_t size)
    : m_Size(size) {
    glGenBuffers(1, &m_RendererID);
    glBindBuffer(GL_ARRAY_BUFFER, m_RendererID);
    glBufferData(GL_ARRAY_BUFFER, size, vertices, vertices ? GL_STATIC_DRAW : GL_DYNAMIC_DRAW);
    VE_GPU_TRACK(GPUResourceType::VertexBuffer, m_RendererID);
}

OpenGLVertexBuffer::~OpenGLVertexBuffer() {
    VE_GPU_UNTRACK(GPUResourceType::VertexBuffer, m_RendererID);
    glDeleteBuffers(1, &m_RendererID);
}

void OpenGLVertexBuffer::Bind() const {
    glBindBuffer(GL_ARRAY_BUFFER, m_RendererID);
}

void OpenGLVertexBuffer::Unbind() const {
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void OpenGLVertexBuffer::SetData(const void* data, uint32_t size) {
    glBindBuffer(GL_ARRAY_BUFFER, m_RendererID);
    glBufferSubData(GL_ARRAY_BUFFER, 0, size, data);
    m_Size = size;
}

// ── IndexBuffer ─────────────────────────────────────────────────────

OpenGLIndexBuffer::OpenGLIndexBuffer(const uint32_t* indices, uint32_t count)
    : m_Count(count) {
    glGenBuffers(1, &m_RendererID);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_RendererID);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, count * sizeof(uint32_t), indices, GL_STATIC_DRAW);
    VE_GPU_TRACK(GPUResourceType::IndexBuffer, m_RendererID);
}

OpenGLIndexBuffer::~OpenGLIndexBuffer() {
    VE_GPU_UNTRACK(GPUResourceType::IndexBuffer, m_RendererID);
    glDeleteBuffers(1, &m_RendererID);
}

void OpenGLIndexBuffer::Bind() const {
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_RendererID);
}

void OpenGLIndexBuffer::Unbind() const {
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

} // namespace VE
