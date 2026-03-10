#pragma once

#include "VibeEngine/Renderer/Buffer.h"
#include <vulkan/vulkan.h>

namespace VE {

class VulkanVertexBuffer : public VertexBuffer {
public:
    VulkanVertexBuffer(const float* vertices, uint32_t size);
    ~VulkanVertexBuffer() override;

    void Bind() const override {}
    void Unbind() const override {}
    void SetData(const void*, uint32_t) override {}
    void SetLayout(const BufferLayout& layout) override { m_Layout = layout; }
    const BufferLayout& GetLayout() const override { return m_Layout; }

    VkBuffer GetVkBuffer() const { return m_Buffer; }

private:
    VkBuffer       m_Buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_Memory = VK_NULL_HANDLE;
    BufferLayout   m_Layout;
};

class VulkanIndexBuffer : public IndexBuffer {
public:
    VulkanIndexBuffer(const uint32_t* indices, uint32_t count);
    ~VulkanIndexBuffer() override;

    void Bind() const override {}
    void Unbind() const override {}
    uint32_t GetCount() const override { return m_Count; }

    VkBuffer GetVkBuffer() const { return m_Buffer; }

private:
    VkBuffer       m_Buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_Memory = VK_NULL_HANDLE;
    uint32_t       m_Count  = 0;
};

} // namespace VE
