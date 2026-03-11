#include "VibeEngine/Platform/Vulkan/VulkanRendererAPI.h"
#include "VibeEngine/Platform/Vulkan/VulkanContext.h"
#include "VibeEngine/Platform/Vulkan/VulkanBuffer.h"
#include "VibeEngine/Renderer/VertexArray.h"
#include "VibeEngine/Core/Log.h"

namespace VE {

void VulkanRendererAPI::Init() {
    VE_ENGINE_INFO("VulkanRendererAPI initialized");
}

void VulkanRendererAPI::SetViewport(uint32_t, uint32_t, uint32_t, uint32_t) {
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

    auto* vkVB = static_cast<VulkanVertexBuffer*>(vbs[0].get());
    auto* vkIB = static_cast<VulkanIndexBuffer*>(ib.get());

    auto& ctx = VulkanContext::Get();

    VulkanDrawCommand cmd{};
    cmd.VertexBuffer         = vkVB->GetVkBuffer();
    cmd.IndexBuffer          = vkIB->GetVkBuffer();
    cmd.IndexCount           = vkIB->GetCount();
    cmd.MVP                  = ctx.GetCurrentMVP();
    cmd.Model                = ctx.GetCurrentModel();
    cmd.UseLitPipeline       = ctx.GetCurrentUseLit();
    cmd.TextureDescriptorSet = ctx.GetCurrentTextureDescriptorSet();
    cmd.UseTexture           = ctx.GetCurrentUseTexture();
    cmd.LightDir             = ctx.GetCurrentLightDir();
    cmd.LightColor           = ctx.GetCurrentLightColor();
    cmd.LightIntensity       = ctx.GetCurrentLightIntensity();
    cmd.ViewPos              = ctx.GetCurrentViewPos();
    cmd.EntityColor          = ctx.GetCurrentEntityColor();
    cmd.UseSkyPipeline       = ctx.GetCurrentUseSky();
    cmd.SkyTopColor          = ctx.GetCurrentSkyTopColor();
    cmd.SkyBottomColor       = ctx.GetCurrentSkyBottomColor();

    ctx.SubmitDrawCommand(cmd);

    // Reset per-draw state
    ctx.SetCurrentUseLit(false);
    ctx.SetCurrentUseSky(false);
    ctx.SetCurrentTextureDescriptorSet(VK_NULL_HANDLE);
    ctx.SetCurrentUseTexture(0);
}

void VulkanRendererAPI::DrawLines(const std::shared_ptr<VertexArray>&, uint32_t) {
    // Not yet implemented for Vulkan
}

void VulkanRendererAPI::SetLineWidth(float) {
    // Not yet implemented for Vulkan
}

void VulkanRendererAPI::SetDepthFunc(DepthFunc) {
    // Vulkan depth func is set per-pipeline, not dynamically
}

void VulkanRendererAPI::SetDepthWrite(bool) {
    // Vulkan depth write is set per-pipeline, not dynamically
}

} // namespace VE
