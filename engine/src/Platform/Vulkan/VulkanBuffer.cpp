#include "VibeEngine/Platform/Vulkan/VulkanBuffer.h"
#include "VibeEngine/Platform/Vulkan/VulkanContext.h"
#include "VibeEngine/Core/Log.h"

namespace VE {

// ── Helper ──────────────────────────────────────────────────────────

static void CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                         VkBuffer& buffer, VkDeviceMemory& memory) {
    auto& ctx    = VulkanContext::Get();
    auto  device = ctx.GetDevice();

    VkBufferCreateInfo ci{};
    ci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size        = size;
    ci.usage       = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device, &ci, nullptr, &buffer) != VK_SUCCESS) {
        VE_ENGINE_ERROR("Failed to create Vulkan buffer!");
        return;
    }

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(device, buffer, &memReqs);

    VkMemoryAllocateInfo ai{};
    ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize  = memReqs.size;
    ai.memoryTypeIndex = ctx.FindMemoryType(memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(device, &ai, nullptr, &memory) != VK_SUCCESS) {
        VE_ENGINE_ERROR("Failed to allocate Vulkan buffer memory!");
        return;
    }

    vkBindBufferMemory(device, buffer, memory, 0);
}

// ── VulkanVertexBuffer ──────────────────────────────────────────────

VulkanVertexBuffer::VulkanVertexBuffer(const float* vertices, uint32_t size)
    : m_Size(size) {
    CreateBuffer(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, m_Buffer, m_Memory);

    void* data;
    vkMapMemory(VulkanContext::Get().GetDevice(), m_Memory, 0, size, 0, &data);
    memcpy(data, vertices, size);
    vkUnmapMemory(VulkanContext::Get().GetDevice(), m_Memory);
}

VulkanVertexBuffer::~VulkanVertexBuffer() {
    auto device = VulkanContext::Get().GetDevice();
    vkDestroyBuffer(device, m_Buffer, nullptr);
    vkFreeMemory(device, m_Memory, nullptr);
}

// ── VulkanIndexBuffer ───────────────────────────────────────────────

VulkanIndexBuffer::VulkanIndexBuffer(const uint32_t* indices, uint32_t count)
    : m_Count(count) {
    VkDeviceSize size = count * sizeof(uint32_t);
    CreateBuffer(size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, m_Buffer, m_Memory);

    void* data;
    vkMapMemory(VulkanContext::Get().GetDevice(), m_Memory, 0, size, 0, &data);
    memcpy(data, indices, size);
    vkUnmapMemory(VulkanContext::Get().GetDevice(), m_Memory);
}

VulkanIndexBuffer::~VulkanIndexBuffer() {
    auto device = VulkanContext::Get().GetDevice();
    vkDestroyBuffer(device, m_Buffer, nullptr);
    vkFreeMemory(device, m_Memory, nullptr);
}

} // namespace VE
