#pragma once

#include "VibeEngine/Renderer/Texture.h"
#include <vulkan/vulkan.h>
#include <string>

namespace VE {

class VulkanTexture2D : public Texture2D {
public:
    explicit VulkanTexture2D(const std::string& path);
    // Create a 1x1 solid-color texture (used as default "no texture")
    VulkanTexture2D(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    ~VulkanTexture2D() override;

    uint32_t GetWidth() const override { return m_Width; }
    uint32_t GetHeight() const override { return m_Height; }
    void Bind(uint32_t slot = 0) const override;
    void Unbind() const override {}
    uint64_t GetNativeTextureID() const override { return reinterpret_cast<uint64_t>(m_DescriptorSet); }

    VkDescriptorSet GetDescriptorSet() const { return m_DescriptorSet; }

private:
    void CreateFromPixels(const unsigned char* pixels, int width, int height, int channels);
    void TransitionImageLayout(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout);
    VkCommandBuffer BeginSingleTimeCommands();
    void EndSingleTimeCommands(VkCommandBuffer commandBuffer);

    uint32_t m_Width  = 0;
    uint32_t m_Height = 0;
    std::string m_FilePath;

    VkImage        m_Image       = VK_NULL_HANDLE;
    VkDeviceMemory m_ImageMemory = VK_NULL_HANDLE;
    VkImageView    m_ImageView   = VK_NULL_HANDLE;
    VkSampler      m_Sampler     = VK_NULL_HANDLE;
    VkDescriptorSet m_DescriptorSet = VK_NULL_HANDLE;
};

} // namespace VE
