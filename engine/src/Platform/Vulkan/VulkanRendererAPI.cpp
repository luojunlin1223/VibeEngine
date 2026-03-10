#include "VibeEngine/Platform/Vulkan/VulkanRendererAPI.h"
#include "VibeEngine/Platform/Vulkan/VulkanContext.h"
#include "VibeEngine/Platform/Vulkan/VulkanBuffer.h"
#include "VibeEngine/Renderer/VertexArray.h"
#include "VibeEngine/Core/Log.h"

namespace VE {

void VulkanRendererAPI::Init() {
    VE_ENGINE_INFO("VulkanRendererAPI initialized");
}

void VulkanRendererAPI::SetViewport(uint32_t /*x*/, uint32_t /*y*/, uint32_t /*width*/, uint32_t /*height*/) {
    // Dynamic viewport is set during command buffer recording
}

void VulkanRendererAPI::SetClearColor(float r, float g, float b, float a) {
    VulkanContext::Get().SetClearColor(r, g, b, a);
}

void VulkanRendererAPI::Clear() {
    // Clear is handled by VK_ATTACHMENT_LOAD_OP_CLEAR in the render pass
}

void VulkanRendererAPI::DrawIndexed(const std::shared_ptr<VertexArray>& vertexArray) {
    if (!vertexArray) return;

    auto& vbs = vertexArray->GetVertexBuffers();
    auto& ib  = vertexArray->GetIndexBuffer();
    if (vbs.empty() || !ib) return;

    // Extract Vulkan-native handles
    auto* vkVB = static_cast<VulkanVertexBuffer*>(vbs[0].get());
    auto* vkIB = static_cast<VulkanIndexBuffer*>(ib.get());

    VulkanDrawCommand cmd{};
    cmd.VertexBuffer = vkVB->GetVkBuffer();
    cmd.IndexBuffer  = vkIB->GetVkBuffer();
    cmd.IndexCount   = vkIB->GetCount();

    VulkanContext::Get().SubmitDrawCommand(cmd);
}

} // namespace VE
