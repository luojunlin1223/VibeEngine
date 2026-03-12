#pragma once

#include "VibeEngine/Renderer/VertexArray.h"

namespace VE {

class VulkanVertexArray : public VertexArray {
public:
    void Bind() const override {}
    void Unbind() const override {}

    void AddVertexBuffer(const std::shared_ptr<VertexBuffer>& vb) override { m_VertexBuffers.push_back(vb); }
    void SetIndexBuffer(const std::shared_ptr<IndexBuffer>& ib) override { m_IndexBuffer = ib; }

    const std::vector<std::shared_ptr<VertexBuffer>>& GetVertexBuffers() const override { return m_VertexBuffers; }
    const std::shared_ptr<IndexBuffer>& GetIndexBuffer() const override { return m_IndexBuffer; }

private:
    std::vector<std::shared_ptr<VertexBuffer>> m_VertexBuffers;
    std::shared_ptr<IndexBuffer> m_IndexBuffer;
};

} // namespace VE
