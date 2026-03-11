#include "VibeEngine/Platform/Vulkan/VulkanTexture.h"
#include "VibeEngine/Platform/Vulkan/VulkanContext.h"
#include "VibeEngine/Core/Log.h"

#include <stb_image.h>
#include <cstring>

namespace VE {

// ── Helper: single-time command buffer ──────────────────────────────

VkCommandBuffer VulkanTexture2D::BeginSingleTimeCommands() {
    auto& ctx = VulkanContext::Get();

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool        = ctx.GetCommandPool();
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(ctx.GetDevice(), &allocInfo, &commandBuffer);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    return commandBuffer;
}

void VulkanTexture2D::EndSingleTimeCommands(VkCommandBuffer commandBuffer) {
    auto& ctx = VulkanContext::Get();

    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &commandBuffer;

    vkQueueSubmit(ctx.GetGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(ctx.GetGraphicsQueue());

    vkFreeCommandBuffers(ctx.GetDevice(), ctx.GetCommandPool(), 1, &commandBuffer);
}

// ── Image layout transition ─────────────────────────────────────────

void VulkanTexture2D::TransitionImageLayout(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout) {
    VkCommandBuffer cmd = BeginSingleTimeCommands();

    VkImageMemoryBarrier barrier{};
    barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout                       = oldLayout;
    barrier.newLayout                       = newLayout;
    barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.image                           = image;
    barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel   = 0;
    barrier.subresourceRange.levelCount     = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount     = 1;

    VkPipelineStageFlags srcStage, dstStage;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else {
        VE_ENGINE_ERROR("Unsupported image layout transition!");
        EndSingleTimeCommands(cmd);
        return;
    }

    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    EndSingleTimeCommands(cmd);
}

// ── Shared pixel upload ─────────────────────────────────────────────

void VulkanTexture2D::CreateFromPixels(const unsigned char* pixels, int width, int height, int channels) {
    auto& ctx = VulkanContext::Get();
    VkDevice device = ctx.GetDevice();

    m_Width  = static_cast<uint32_t>(width);
    m_Height = static_cast<uint32_t>(height);

    VkDeviceSize imageSize = static_cast<VkDeviceSize>(width) * height * 4; // Always RGBA

    // Convert to RGBA if needed
    std::vector<unsigned char> rgbaData;
    const unsigned char* uploadData = pixels;
    if (channels != 4) {
        rgbaData.resize(width * height * 4);
        for (int i = 0; i < width * height; i++) {
            if (channels == 3) {
                rgbaData[i * 4 + 0] = pixels[i * 3 + 0];
                rgbaData[i * 4 + 1] = pixels[i * 3 + 1];
                rgbaData[i * 4 + 2] = pixels[i * 3 + 2];
                rgbaData[i * 4 + 3] = 255;
            } else if (channels == 1) {
                rgbaData[i * 4 + 0] = pixels[i];
                rgbaData[i * 4 + 1] = pixels[i];
                rgbaData[i * 4 + 2] = pixels[i];
                rgbaData[i * 4 + 3] = 255;
            }
        }
        uploadData = rgbaData.data();
    }

    // Staging buffer
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size  = imageSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    vkCreateBuffer(device, &bufferInfo, nullptr, &stagingBuffer);

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(device, stagingBuffer, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize  = memReqs.size;
    allocInfo.memoryTypeIndex = ctx.FindMemoryType(memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(device, &allocInfo, nullptr, &stagingMemory);
    vkBindBufferMemory(device, stagingBuffer, stagingMemory, 0);

    void* data;
    vkMapMemory(device, stagingMemory, 0, imageSize, 0, &data);
    std::memcpy(data, uploadData, static_cast<size_t>(imageSize));
    vkUnmapMemory(device, stagingMemory);

    // Create image
    VkImageCreateInfo imageInfo{};
    imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType     = VK_IMAGE_TYPE_2D;
    imageInfo.format        = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent.width  = m_Width;
    imageInfo.extent.height = m_Height;
    imageInfo.extent.depth  = 1;
    imageInfo.mipLevels     = 1;
    imageInfo.arrayLayers   = 1;
    imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    if (vkCreateImage(device, &imageInfo, nullptr, &m_Image) != VK_SUCCESS) {
        VE_ENGINE_ERROR("Failed to create Vulkan texture image!");
        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkFreeMemory(device, stagingMemory, nullptr);
        return;
    }

    VkMemoryRequirements imgMemReqs;
    vkGetImageMemoryRequirements(device, m_Image, &imgMemReqs);

    VkMemoryAllocateInfo imgAllocInfo{};
    imgAllocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    imgAllocInfo.allocationSize  = imgMemReqs.size;
    imgAllocInfo.memoryTypeIndex = ctx.FindMemoryType(imgMemReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(device, &imgAllocInfo, nullptr, &m_ImageMemory);
    vkBindImageMemory(device, m_Image, m_ImageMemory, 0);

    // Transition + copy + transition
    TransitionImageLayout(m_Image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkCommandBuffer cmd = BeginSingleTimeCommands();
    VkBufferImageCopy region{};
    region.bufferOffset      = 0;
    region.bufferRowLength   = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel       = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount     = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {m_Width, m_Height, 1};
    vkCmdCopyBufferToImage(cmd, stagingBuffer, m_Image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    EndSingleTimeCommands(cmd);

    TransitionImageLayout(m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // Cleanup staging
    vkDestroyBuffer(device, stagingBuffer, nullptr);
    vkFreeMemory(device, stagingMemory, nullptr);

    // Image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image    = m_Image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format   = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel   = 0;
    viewInfo.subresourceRange.levelCount     = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount     = 1;

    if (vkCreateImageView(device, &viewInfo, nullptr, &m_ImageView) != VK_SUCCESS) {
        VE_ENGINE_ERROR("Failed to create texture image view!");
        return;
    }

    // Sampler
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter    = VK_FILTER_LINEAR;
    samplerInfo.minFilter    = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;

    if (vkCreateSampler(device, &samplerInfo, nullptr, &m_Sampler) != VK_SUCCESS) {
        VE_ENGINE_ERROR("Failed to create texture sampler!");
        return;
    }

    // Allocate and write descriptor set
    m_DescriptorSet = ctx.AllocateTextureDescriptorSet(m_ImageView, m_Sampler);
}

// ── Constructor: load from file ─────────────────────────────────────

VulkanTexture2D::VulkanTexture2D(const std::string& path)
    : m_FilePath(path)
{
    // Vulkan uses top-down image coordinates, no vertical flip needed
    stbi_set_flip_vertically_on_load(false);

    int width, height, channels;
    unsigned char* data = stbi_load(path.c_str(), &width, &height, &channels, 0);
    if (!data) {
        VE_ENGINE_ERROR("Failed to load texture: {0}", path);
        return;
    }

    CreateFromPixels(data, width, height, channels);
    stbi_image_free(data);

    VE_ENGINE_INFO("Vulkan texture loaded: {0} ({1}x{2}, {3}ch)", path, m_Width, m_Height, channels);
}

// ── Constructor: solid color ────────────────────────────────────────

VulkanTexture2D::VulkanTexture2D(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    unsigned char pixel[4] = { r, g, b, a };
    CreateFromPixels(pixel, 1, 1, 4);
}

// ── Destructor ──────────────────────────────────────────────────────

VulkanTexture2D::~VulkanTexture2D() {
    VkDevice device = VulkanContext::Get().GetDevice();
    vkDeviceWaitIdle(device);

    if (m_Sampler)     vkDestroySampler(device, m_Sampler, nullptr);
    if (m_ImageView)   vkDestroyImageView(device, m_ImageView, nullptr);
    if (m_Image)       vkDestroyImage(device, m_Image, nullptr);
    if (m_ImageMemory) vkFreeMemory(device, m_ImageMemory, nullptr);
    // Descriptor set is freed when the pool is destroyed/reset
}

// ── Bind (sets current texture descriptor in context) ───────────────

void VulkanTexture2D::Bind(uint32_t /*slot*/) const {
    VulkanContext::Get().SetCurrentTextureDescriptorSet(m_DescriptorSet);
}

} // namespace VE
