#pragma once

#include "VibeEngine/Renderer/Buffer.h"

namespace VE {

class OpenGLVertexBuffer : public VertexBuffer {
public:
    OpenGLVertexBuffer(const float* vertices, uint32_t size);
    ~OpenGLVertexBuffer() override;

    void Bind() const override;
    void Unbind() const override;
    void SetData(const void* data, uint32_t size) override;
    void SetLayout(const BufferLayout& layout) override { m_Layout = layout; }
    const BufferLayout& GetLayout() const override { return m_Layout; }
    uint32_t GetSize() const override { return m_Size; }

private:
    uint32_t     m_RendererID = 0;
    uint32_t     m_Size = 0;
    BufferLayout m_Layout;
};

class OpenGLIndexBuffer : public IndexBuffer {
public:
    OpenGLIndexBuffer(const uint32_t* indices, uint32_t count);
    ~OpenGLIndexBuffer() override;

    void Bind() const override;
    void Unbind() const override;
    uint32_t GetCount() const override { return m_Count; }

private:
    uint32_t m_RendererID = 0;
    uint32_t m_Count      = 0;
};

} // namespace VE
