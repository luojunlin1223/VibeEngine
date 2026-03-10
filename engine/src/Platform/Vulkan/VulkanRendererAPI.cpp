#include "VibeEngine/Platform/Vulkan/VulkanRendererAPI.h"
#include "VibeEngine/Core/Log.h"

namespace VE {

void VulkanRendererAPI::Init() {
    VE_ENGINE_INFO("VulkanRendererAPI::Init - stub (rendering via VulkanContext)");
}

void VulkanRendererAPI::SetViewport(uint32_t /*x*/, uint32_t /*y*/, uint32_t /*width*/, uint32_t /*height*/) {
    // TODO: Record viewport command into command buffer
}

void VulkanRendererAPI::SetClearColor(float /*r*/, float /*g*/, float /*b*/, float /*a*/) {
    // TODO: Store clear color for render pass begin
}

void VulkanRendererAPI::Clear() {
    // TODO: Handled by render pass clear values
}

void VulkanRendererAPI::DrawIndexed(const std::shared_ptr<VertexArray>& /*vertexArray*/) {
    // TODO: Record draw commands into command buffer
}

} // namespace VE
