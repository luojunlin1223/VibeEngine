#include "VibeEngine/Platform/Vulkan/VulkanContext.h"
#include "VibeEngine/Platform/Vulkan/TriangleShadersSpv.h"
#include "VibeEngine/Platform/Vulkan/LitShadersSpv.h"
#include "VibeEngine/Platform/Vulkan/SkyShadersSpv.h"
#include "VibeEngine/Core/Log.h"

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <glm/glm.hpp>

#include <GLFW/glfw3.h>
#include <vector>
#include <set>
#include <algorithm>
#include <array>

namespace VE {

VulkanContext* VulkanContext::s_Instance = nullptr;

VulkanContext::VulkanContext(GLFWwindow* windowHandle)
    : m_WindowHandle(windowHandle) {}

VulkanContext::~VulkanContext() {
    Cleanup();
}

void VulkanContext::Init() {
    s_Instance = this;
    CreateInstance();
    CreateSurface();
    PickPhysicalDevice();
    CreateLogicalDevice();
    CreateSwapchain();
    CreateImageViews();
    CreateDepthResources();
    CreateRenderPass();
    CreateDescriptorSetLayout();
    CreateDescriptorPool();
    CreateGraphicsPipeline();
    CreateLitGraphicsPipeline();
    CreateSkyGraphicsPipeline();
    CreateFramebuffers();
    CreateCommandPool();
    CreateCommandBuffers();
    CreateSyncObjects();
    CreateDefaultTexture();
    VE_ENGINE_INFO("Vulkan context initialized successfully");
}

void VulkanContext::SetClearColor(float r, float g, float b, float a) {
    m_ClearColor = {{r, g, b, a}};
}

void VulkanContext::SubmitDrawCommand(const VulkanDrawCommand& cmd) {
    m_DrawCommands.push_back(cmd);
}

// ── SwapBuffers (frame presentation) ────────────────────────────────

void VulkanContext::RecreateSwapchain() {
    int width = 0, height = 0;
    glfwGetFramebufferSize(m_WindowHandle, &width, &height);
    while (width == 0 || height == 0) {
        glfwGetFramebufferSize(m_WindowHandle, &width, &height);
        glfwWaitEvents();
    }

    vkDeviceWaitIdle(m_Device);

    CleanupSwapchain();
    CreateSwapchain();
    CreateImageViews();
    CreateDepthResources();
    CreateFramebuffers();

    VE_ENGINE_INFO("Vulkan swapchain recreated ({0}x{1})", m_SwapchainExtent.width, m_SwapchainExtent.height);
}

void VulkanContext::SwapBuffers() {
    vkWaitForFences(m_Device, 1, &m_InFlightFences[m_CurrentFrame], VK_TRUE, UINT64_MAX);

    uint32_t imageIndex;
    VkResult result = vkAcquireNextImageKHR(m_Device, m_Swapchain, UINT64_MAX,
                                            m_ImageAvailableSemaphores[m_CurrentFrame],
                                            VK_NULL_HANDLE, &imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        RecreateSwapchain();
        return;
    }

    vkResetFences(m_Device, 1, &m_InFlightFences[m_CurrentFrame]);
    vkResetCommandBuffer(m_CommandBuffers[m_CurrentFrame], 0);
    RecordCommandBuffer(m_CommandBuffers[m_CurrentFrame], imageIndex);

    VkSemaphore waitSemaphores[]   = { m_ImageAvailableSemaphores[m_CurrentFrame] };
    VkSemaphore signalSemaphores[] = { m_RenderFinishedSemaphores[m_CurrentFrame] };
    VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };

    VkSubmitInfo submitInfo{};
    submitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount   = 1;
    submitInfo.pWaitSemaphores      = waitSemaphores;
    submitInfo.pWaitDstStageMask    = waitStages;
    submitInfo.commandBufferCount   = 1;
    submitInfo.pCommandBuffers      = &m_CommandBuffers[m_CurrentFrame];
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores    = signalSemaphores;

    if (vkQueueSubmit(m_GraphicsQueue, 1, &submitInfo, m_InFlightFences[m_CurrentFrame]) != VK_SUCCESS) {
        VE_ENGINE_ERROR("Failed to submit Vulkan draw command buffer!");
    }

    VkSwapchainKHR swapchains[] = { m_Swapchain };
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores    = signalSemaphores;
    presentInfo.swapchainCount     = 1;
    presentInfo.pSwapchains        = swapchains;
    presentInfo.pImageIndices      = &imageIndex;

    result = vkQueuePresentKHR(m_PresentQueue, &presentInfo);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        RecreateSwapchain();
    }

    m_CurrentFrame = (m_CurrentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
}

void VulkanContext::RecordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &beginInfo);

    // Two clear values: color + depth
    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = m_ClearColor;
    clearValues[1].depthStencil = { 1.0f, 0 };

    VkRenderPassBeginInfo rpInfo{};
    rpInfo.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpInfo.renderPass        = m_RenderPass;
    rpInfo.framebuffer       = m_Framebuffers[imageIndex];
    rpInfo.renderArea.offset = {0, 0};
    rpInfo.renderArea.extent = m_SwapchainExtent;
    rpInfo.clearValueCount   = static_cast<uint32_t>(clearValues.size());
    rpInfo.pClearValues      = clearValues.data();

    vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

    // Dynamic viewport (Y-flip) and scissor
    VkViewport viewport{};
    viewport.x        = 0.0f;
    viewport.y        = static_cast<float>(m_SwapchainExtent.height);
    viewport.width    = static_cast<float>(m_SwapchainExtent.width);
    viewport.height   = -static_cast<float>(m_SwapchainExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = m_SwapchainExtent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Draw all submitted draw commands, switching pipelines as needed
    VkPipeline currentPipeline = VK_NULL_HANDLE;

    for (const auto& dc : m_DrawCommands) {
        VkPipeline requiredPipeline;
        VkPipelineLayout layout;
        if (dc.UseSkyPipeline) {
            requiredPipeline = m_SkyGraphicsPipeline;
            layout = m_SkyPipelineLayout;
        } else if (dc.UseLitPipeline) {
            requiredPipeline = m_LitGraphicsPipeline;
            layout = m_LitPipelineLayout;
        } else {
            requiredPipeline = m_GraphicsPipeline;
            layout = m_PipelineLayout;
        }

        if (requiredPipeline != currentPipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, requiredPipeline);
            currentPipeline = requiredPipeline;
        }

        // Bind texture descriptor set (default white if none provided)
        VkDescriptorSet texDS = dc.TextureDescriptorSet ? dc.TextureDescriptorSet : m_DefaultTextureDS;
        if (texDS) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    layout, 0, 1, &texDS, 0, nullptr);
        }

        if (dc.UseSkyPipeline) {
            // Push sky VP (64 bytes vertex) + sky colors + useTexture (fragment)
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(glm::mat4), &dc.MVP);
            struct SkyFragPC {
                float topColor[3];       // offset 64
                float _pad1;             // padding
                float bottomColor[3];    // offset 80
                int32_t useTexture;      // offset 92
            } skyFrag{};
            skyFrag.topColor[0] = dc.SkyTopColor.x; skyFrag.topColor[1] = dc.SkyTopColor.y; skyFrag.topColor[2] = dc.SkyTopColor.z;
            skyFrag.bottomColor[0] = dc.SkyBottomColor.x; skyFrag.bottomColor[1] = dc.SkyBottomColor.y; skyFrag.bottomColor[2] = dc.SkyBottomColor.z;
            skyFrag.useTexture = dc.UseTexture;
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                               64, sizeof(skyFrag), &skyFrag);
        } else if (dc.UseLitPipeline) {
            // Push MVP + Model (128 bytes vertex)
            struct { glm::mat4 mvp; glm::mat4 model; } vertPC = { dc.MVP, dc.Model };
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(vertPC), &vertPC);
            // Push fragment data: useTexture + lighting (80 bytes at offset 128)
            struct LitFragPC {
                int32_t useTexture;           // offset 128
                float   _pad1[3];
                float   lightDir[3];          // offset 144
                float   _pad2;
                float   lightColor[3];        // offset 160
                float   lightIntensity;       // offset 172
                float   viewPos[3];           // offset 176
                float   _pad3;
                float   entityColor[4];       // offset 192
                float   metallic;             // offset 208
                float   roughness;            // offset 212
                float   ao;                   // offset 216
                float   _pad4;                // offset 220
            } fragPC{};
            fragPC.useTexture = dc.UseTexture;
            fragPC.lightDir[0] = dc.LightDir.x;   fragPC.lightDir[1] = dc.LightDir.y;   fragPC.lightDir[2] = dc.LightDir.z;
            fragPC.lightColor[0] = dc.LightColor.x; fragPC.lightColor[1] = dc.LightColor.y; fragPC.lightColor[2] = dc.LightColor.z;
            fragPC.lightIntensity = dc.LightIntensity;
            fragPC.viewPos[0] = dc.ViewPos.x;     fragPC.viewPos[1] = dc.ViewPos.y;     fragPC.viewPos[2] = dc.ViewPos.z;
            fragPC.entityColor[0] = dc.EntityColor.x; fragPC.entityColor[1] = dc.EntityColor.y;
            fragPC.entityColor[2] = dc.EntityColor.z; fragPC.entityColor[3] = dc.EntityColor.w;
            fragPC.metallic = dc.Metallic;
            fragPC.roughness = dc.Roughness;
            fragPC.ao = dc.AO;
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                               128, sizeof(fragPC), &fragPC);
        } else {
            // Push MVP (64 bytes vertex) + useTexture (4 bytes fragment)
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(glm::mat4), &dc.MVP);
            int32_t useTexture = dc.UseTexture;
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                               64, sizeof(int32_t), &useTexture);
        }

        VkBuffer vertexBuffers[] = { dc.VertexBuffer };
        VkDeviceSize offsets[]   = { 0 };
        vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
        vkCmdBindIndexBuffer(cmd, dc.IndexBuffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, dc.IndexCount, 1, 0, 0, 0);
    }

    // Render ImGui
    if (m_ImGuiEnabled) {
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    }

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    m_DrawCommands.clear();
}

// ── Instance ────────────────────────────────────────────────────────

void VulkanContext::CreateInstance() {
    VkApplicationInfo appInfo{};
    appInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName   = "VibeEngine";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 2, 0);
    appInfo.pEngineName        = "VibeEngine";
    appInfo.engineVersion      = VK_MAKE_VERSION(0, 2, 0);
    appInfo.apiVersion         = VK_API_VERSION_1_3;

    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);

#ifdef VE_DEBUG
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#endif

    VkInstanceCreateInfo createInfo{};
    createInfo.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo        = &appInfo;
    createInfo.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

#ifdef VE_DEBUG
    const char* validationLayer = "VK_LAYER_KHRONOS_validation";
    createInfo.enabledLayerCount   = 1;
    createInfo.ppEnabledLayerNames = &validationLayer;
#else
    createInfo.enabledLayerCount = 0;
#endif

    if (vkCreateInstance(&createInfo, nullptr, &m_Instance) != VK_SUCCESS) {
        VE_ENGINE_ERROR("Failed to create Vulkan instance!");
        return;
    }
    VE_ENGINE_INFO("Vulkan instance created");
}

void VulkanContext::CreateSurface() {
    if (glfwCreateWindowSurface(m_Instance, m_WindowHandle, nullptr, &m_Surface) != VK_SUCCESS) {
        VE_ENGINE_ERROR("Failed to create Vulkan window surface!");
    }
}

// ── Physical / Logical device ───────────────────────────────────────

void VulkanContext::PickPhysicalDevice() {
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(m_Instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        VE_ENGINE_ERROR("No Vulkan-capable GPU found!");
        return;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(m_Instance, &deviceCount, devices.data());

    for (const auto& device : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(device, &props);

        m_PhysicalDevice = device;
        m_GraphicsFamily = FindQueueFamily(VK_QUEUE_GRAPHICS_BIT);

        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
        for (uint32_t i = 0; i < queueFamilyCount; i++) {
            VkBool32 presentSupport = false;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_Surface, &presentSupport);
            if (presentSupport) {
                m_PresentFamily = static_cast<int>(i);
                break;
            }
        }

        if (m_GraphicsFamily >= 0 && m_PresentFamily >= 0) {
            VE_ENGINE_INFO("Vulkan GPU: {0}", props.deviceName);
            return;
        }
    }
    VE_ENGINE_ERROR("No suitable Vulkan GPU found!");
}

void VulkanContext::CreateLogicalDevice() {
    std::set<int> uniqueFamilies = { m_GraphicsFamily, m_PresentFamily };
    std::vector<VkDeviceQueueCreateInfo> queueInfos;
    float priority = 1.0f;

    for (int family : uniqueFamilies) {
        VkDeviceQueueCreateInfo qi{};
        qi.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi.queueFamilyIndex = static_cast<uint32_t>(family);
        qi.queueCount       = 1;
        qi.pQueuePriorities = &priority;
        queueInfos.push_back(qi);
    }

    VkPhysicalDeviceFeatures features{};
    const char* ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;

    VkDeviceCreateInfo ci{};
    ci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount    = static_cast<uint32_t>(queueInfos.size());
    ci.pQueueCreateInfos       = queueInfos.data();
    ci.pEnabledFeatures        = &features;
    ci.enabledExtensionCount   = 1;
    ci.ppEnabledExtensionNames = &ext;

    if (vkCreateDevice(m_PhysicalDevice, &ci, nullptr, &m_Device) != VK_SUCCESS) {
        VE_ENGINE_ERROR("Failed to create Vulkan logical device!");
        return;
    }
    vkGetDeviceQueue(m_Device, static_cast<uint32_t>(m_GraphicsFamily), 0, &m_GraphicsQueue);
    vkGetDeviceQueue(m_Device, static_cast<uint32_t>(m_PresentFamily),  0, &m_PresentQueue);
    VE_ENGINE_INFO("Vulkan logical device created");
}

int VulkanContext::FindQueueFamily(VkQueueFlagBits flags) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_PhysicalDevice, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(m_PhysicalDevice, &count, families.data());

    for (uint32_t i = 0; i < count; i++) {
        if (families[i].queueFlags & flags)
            return static_cast<int>(i);
    }
    return -1;
}

// ── Swapchain ───────────────────────────────────────────────────────

void VulkanContext::CreateSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_PhysicalDevice, m_Surface, &caps);

    uint32_t formatCount;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_PhysicalDevice, m_Surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_PhysicalDevice, m_Surface, &formatCount, formats.data());

    VkSurfaceFormatKHR surfaceFormat = formats[0];
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            surfaceFormat = f;
            break;
        }
    }
    m_SwapchainFormat = surfaceFormat.format;

    if (caps.currentExtent.width != UINT32_MAX) {
        m_SwapchainExtent = caps.currentExtent;
    } else {
        int w, h;
        glfwGetFramebufferSize(m_WindowHandle, &w, &h);
        m_SwapchainExtent.width  = std::clamp(static_cast<uint32_t>(w), caps.minImageExtent.width,  caps.maxImageExtent.width);
        m_SwapchainExtent.height = std::clamp(static_cast<uint32_t>(h), caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
        imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci{};
    ci.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface          = m_Surface;
    ci.minImageCount    = imageCount;
    ci.imageFormat      = surfaceFormat.format;
    ci.imageColorSpace  = surfaceFormat.colorSpace;
    ci.imageExtent      = m_SwapchainExtent;
    ci.imageArrayLayers = 1;
    ci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.preTransform     = caps.currentTransform;
    ci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode      = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped          = VK_TRUE;

    uint32_t familyIndices[] = {
        static_cast<uint32_t>(m_GraphicsFamily),
        static_cast<uint32_t>(m_PresentFamily)
    };
    if (m_GraphicsFamily != m_PresentFamily) {
        ci.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
        ci.queueFamilyIndexCount = 2;
        ci.pQueueFamilyIndices   = familyIndices;
    } else {
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    if (vkCreateSwapchainKHR(m_Device, &ci, nullptr, &m_Swapchain) != VK_SUCCESS) {
        VE_ENGINE_ERROR("Failed to create Vulkan swapchain!");
        return;
    }

    vkGetSwapchainImagesKHR(m_Device, m_Swapchain, &imageCount, nullptr);
    m_SwapchainImages.resize(imageCount);
    vkGetSwapchainImagesKHR(m_Device, m_Swapchain, &imageCount, m_SwapchainImages.data());

    VE_ENGINE_INFO("Vulkan swapchain created ({0}x{1}, {2} images)",
                   m_SwapchainExtent.width, m_SwapchainExtent.height, imageCount);
}

void VulkanContext::CreateImageViews() {
    m_SwapchainImageViews.resize(m_SwapchainImages.size());
    for (size_t i = 0; i < m_SwapchainImages.size(); i++) {
        VkImageViewCreateInfo ci{};
        ci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ci.image    = m_SwapchainImages[i];
        ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ci.format   = m_SwapchainFormat;
        ci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        ci.subresourceRange.baseMipLevel   = 0;
        ci.subresourceRange.levelCount     = 1;
        ci.subresourceRange.baseArrayLayer = 0;
        ci.subresourceRange.layerCount     = 1;

        if (vkCreateImageView(m_Device, &ci, nullptr, &m_SwapchainImageViews[i]) != VK_SUCCESS) {
            VE_ENGINE_ERROR("Failed to create swapchain image view!");
        }
    }
}

// ── Depth buffer ────────────────────────────────────────────────────

VkFormat VulkanContext::FindDepthFormat() {
    VkFormat candidates[] = { VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT };
    for (VkFormat fmt : candidates) {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(m_PhysicalDevice, fmt, &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
            return fmt;
    }
    return VK_FORMAT_D32_SFLOAT;
}

void VulkanContext::CreateDepthResources() {
    m_DepthFormat = FindDepthFormat();

    VkImageCreateInfo imageInfo{};
    imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType     = VK_IMAGE_TYPE_2D;
    imageInfo.format        = m_DepthFormat;
    imageInfo.extent.width  = m_SwapchainExtent.width;
    imageInfo.extent.height = m_SwapchainExtent.height;
    imageInfo.extent.depth  = 1;
    imageInfo.mipLevels     = 1;
    imageInfo.arrayLayers   = 1;
    imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

    if (vkCreateImage(m_Device, &imageInfo, nullptr, &m_DepthImage) != VK_SUCCESS) {
        VE_ENGINE_ERROR("Failed to create depth image!");
        return;
    }

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(m_Device, m_DepthImage, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize  = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(m_Device, &allocInfo, nullptr, &m_DepthImageMemory) != VK_SUCCESS) {
        VE_ENGINE_ERROR("Failed to allocate depth image memory!");
        return;
    }
    vkBindImageMemory(m_Device, m_DepthImage, m_DepthImageMemory, 0);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image    = m_DepthImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format   = m_DepthFormat;
    viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.baseMipLevel   = 0;
    viewInfo.subresourceRange.levelCount     = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount     = 1;

    if (vkCreateImageView(m_Device, &viewInfo, nullptr, &m_DepthImageView) != VK_SUCCESS) {
        VE_ENGINE_ERROR("Failed to create depth image view!");
    }
}

// ── Render pass ─────────────────────────────────────────────────────

void VulkanContext::CreateRenderPass() {
    // Color attachment
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format         = m_SwapchainFormat;
    colorAttachment.samples        = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    // Depth attachment
    VkAttachmentDescription depthAttachment{};
    depthAttachment.format         = m_DepthFormat;
    depthAttachment.samples        = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 1;
    depthRef.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = 1;
    subpass.pColorAttachments       = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass    = 0;
    dependency.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    std::array<VkAttachmentDescription, 2> attachments = { colorAttachment, depthAttachment };

    VkRenderPassCreateInfo ci{};
    ci.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = static_cast<uint32_t>(attachments.size());
    ci.pAttachments    = attachments.data();
    ci.subpassCount    = 1;
    ci.pSubpasses      = &subpass;
    ci.dependencyCount = 1;
    ci.pDependencies   = &dependency;

    if (vkCreateRenderPass(m_Device, &ci, nullptr, &m_RenderPass) != VK_SUCCESS) {
        VE_ENGINE_ERROR("Failed to create Vulkan render pass!");
    }
}

// ── Graphics pipelines ──────────────────────────────────────────────

VkShaderModule VulkanContext::CreateShaderModule(const uint32_t* code, size_t size) {
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = size;
    ci.pCode    = code;

    VkShaderModule module;
    if (vkCreateShaderModule(m_Device, &ci, nullptr, &module) != VK_SUCCESS) {
        VE_ENGINE_ERROR("Failed to create shader module!");
        return VK_NULL_HANDLE;
    }
    return module;
}

void VulkanContext::CreateGraphicsPipeline() {
    VkShaderModule vertModule = CreateShaderModule(triangle_vert_spv, triangle_vert_spv_size);
    VkShaderModule fragModule = CreateShaderModule(triangle_frag_spv, triangle_frag_spv_size);

    VkPipelineShaderStageCreateInfo shaderStages[2]{};
    shaderStages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[0].module = vertModule;
    shaderStages[0].pName  = "main";
    shaderStages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStages[1].module = fragModule;
    shaderStages[1].pName  = "main";

    // Vertex input: position(float3) + color(float3) + texcoord(float2), stride = 32 bytes
    VkVertexInputBindingDescription bindingDesc{};
    bindingDesc.binding   = 0;
    bindingDesc.stride    = sizeof(float) * 8;
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 3> attrDescs{};
    attrDescs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 };                    // position
    attrDescs[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(float) * 3 };    // color
    attrDescs[2] = { 2, 0, VK_FORMAT_R32G32_SFLOAT,    sizeof(float) * 6 };    // texcoord

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount    = 1;
    vertexInputInfo.pVertexBindingDescriptions       = &bindingDesc;
    vertexInputInfo.vertexAttributeDescriptionCount  = static_cast<uint32_t>(attrDescs.size());
    vertexInputInfo.pVertexAttributeDescriptions     = attrDescs.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates    = dynamicStates;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth   = 1.0f;
    rasterizer.cullMode    = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable  = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp   = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments    = &colorBlendAttachment;

    // Push constants: MVP (vertex, 64 bytes) + useTexture (fragment, 4 bytes at offset 64)
    std::array<VkPushConstantRange, 2> pushConstantRanges{};
    pushConstantRanges[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstantRanges[0].offset     = 0;
    pushConstantRanges[0].size       = sizeof(float) * 16; // mat4 MVP
    pushConstantRanges[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRanges[1].offset     = 64;
    pushConstantRanges[1].size       = sizeof(int32_t); // useTexture

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount         = 1;
    layoutInfo.pSetLayouts            = &m_TextureDescriptorSetLayout;
    layoutInfo.pushConstantRangeCount = static_cast<uint32_t>(pushConstantRanges.size());
    layoutInfo.pPushConstantRanges    = pushConstantRanges.data();
    if (vkCreatePipelineLayout(m_Device, &layoutInfo, nullptr, &m_PipelineLayout) != VK_SUCCESS) {
        VE_ENGINE_ERROR("Failed to create pipeline layout!");
    }

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount          = 2;
    pipelineInfo.pStages             = shaderStages;
    pipelineInfo.pVertexInputState   = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState      = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState   = &multisampling;
    pipelineInfo.pDepthStencilState  = &depthStencil;
    pipelineInfo.pColorBlendState    = &colorBlending;
    pipelineInfo.pDynamicState       = &dynamicState;
    pipelineInfo.layout              = m_PipelineLayout;
    pipelineInfo.renderPass          = m_RenderPass;
    pipelineInfo.subpass             = 0;

    if (vkCreateGraphicsPipelines(m_Device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_GraphicsPipeline) != VK_SUCCESS) {
        VE_ENGINE_ERROR("Failed to create Vulkan graphics pipeline!");
    }

    vkDestroyShaderModule(m_Device, vertModule, nullptr);
    vkDestroyShaderModule(m_Device, fragModule, nullptr);
    VE_ENGINE_INFO("Vulkan unlit pipeline created");
}

void VulkanContext::CreateLitGraphicsPipeline() {
    VkShaderModule vertModule = CreateShaderModule(lit_vert_spv, lit_vert_spv_size);
    VkShaderModule fragModule = CreateShaderModule(lit_frag_spv, lit_frag_spv_size);

    VkPipelineShaderStageCreateInfo shaderStages[2]{};
    shaderStages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[0].module = vertModule;
    shaderStages[0].pName  = "main";
    shaderStages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStages[1].module = fragModule;
    shaderStages[1].pName  = "main";

    // Vertex input: position(3) + normal(3) + color(3) + texcoord(2), stride = 44 bytes
    VkVertexInputBindingDescription bindingDesc{};
    bindingDesc.binding   = 0;
    bindingDesc.stride    = sizeof(float) * 11;
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 4> attrDescs{};
    attrDescs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 };                    // position
    attrDescs[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(float) * 3 };    // normal
    attrDescs[2] = { 2, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(float) * 6 };    // color
    attrDescs[3] = { 3, 0, VK_FORMAT_R32G32_SFLOAT,    sizeof(float) * 9 };    // texcoord

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount    = 1;
    vertexInputInfo.pVertexBindingDescriptions       = &bindingDesc;
    vertexInputInfo.vertexAttributeDescriptionCount  = static_cast<uint32_t>(attrDescs.size());
    vertexInputInfo.pVertexAttributeDescriptions     = attrDescs.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates    = dynamicStates;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth   = 1.0f;
    rasterizer.cullMode    = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable  = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp   = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments    = &colorBlendAttachment;

    // Push constants: MVP+Model (vertex, 128 bytes) + lighting data (fragment, 80 bytes at offset 128)
    std::array<VkPushConstantRange, 2> litPushConstantRanges{};
    litPushConstantRanges[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    litPushConstantRanges[0].offset     = 0;
    litPushConstantRanges[0].size       = sizeof(float) * 32; // 2x mat4 = 128 bytes
    litPushConstantRanges[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    litPushConstantRanges[1].offset     = 128;
    litPushConstantRanges[1].size       = 96; // useTexture + lighting + entityColor + PBR (metallic/roughness/ao)

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount         = 1;
    layoutInfo.pSetLayouts            = &m_TextureDescriptorSetLayout;
    layoutInfo.pushConstantRangeCount = static_cast<uint32_t>(litPushConstantRanges.size());
    layoutInfo.pPushConstantRanges    = litPushConstantRanges.data();
    if (vkCreatePipelineLayout(m_Device, &layoutInfo, nullptr, &m_LitPipelineLayout) != VK_SUCCESS) {
        VE_ENGINE_ERROR("Failed to create lit pipeline layout!");
    }

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount          = 2;
    pipelineInfo.pStages             = shaderStages;
    pipelineInfo.pVertexInputState   = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState      = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState   = &multisampling;
    pipelineInfo.pDepthStencilState  = &depthStencil;
    pipelineInfo.pColorBlendState    = &colorBlending;
    pipelineInfo.pDynamicState       = &dynamicState;
    pipelineInfo.layout              = m_LitPipelineLayout;
    pipelineInfo.renderPass          = m_RenderPass;
    pipelineInfo.subpass             = 0;

    if (vkCreateGraphicsPipelines(m_Device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_LitGraphicsPipeline) != VK_SUCCESS) {
        VE_ENGINE_ERROR("Failed to create Vulkan lit pipeline!");
    }

    vkDestroyShaderModule(m_Device, vertModule, nullptr);
    vkDestroyShaderModule(m_Device, fragModule, nullptr);
    VE_ENGINE_INFO("Vulkan lit pipeline created");
}

void VulkanContext::CreateSkyGraphicsPipeline() {
    VkShaderModule vertModule = CreateShaderModule(sky_vert_spv, sky_vert_spv_size);
    VkShaderModule fragModule = CreateShaderModule(sky_frag_spv, sky_frag_spv_size);

    VkPipelineShaderStageCreateInfo shaderStages[2]{};
    shaderStages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[0].module = vertModule;
    shaderStages[0].pName  = "main";
    shaderStages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStages[1].module = fragModule;
    shaderStages[1].pName  = "main";

    // Vertex input: position(float3) only, stride = 12 bytes
    VkVertexInputBindingDescription bindingDesc{};
    bindingDesc.binding   = 0;
    bindingDesc.stride    = sizeof(float) * 3;
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrDesc{};
    attrDesc = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 };

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount    = 1;
    vertexInputInfo.pVertexBindingDescriptions       = &bindingDesc;
    vertexInputInfo.vertexAttributeDescriptionCount  = 1;
    vertexInputInfo.pVertexAttributeDescriptions     = &attrDesc;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates    = dynamicStates;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth   = 1.0f;
    rasterizer.cullMode    = VK_CULL_MODE_FRONT_BIT; // camera is inside the sphere
    rasterizer.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Sky: depth test LEQUAL, depth write OFF
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable  = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments    = &colorBlendAttachment;

    // Push constants: sky VP (vertex, 64 bytes) + sky colors + useTexture (fragment, 32 bytes)
    std::array<VkPushConstantRange, 2> skyPushConstantRanges{};
    skyPushConstantRanges[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    skyPushConstantRanges[0].offset     = 0;
    skyPushConstantRanges[0].size       = sizeof(float) * 16; // mat4
    skyPushConstantRanges[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    skyPushConstantRanges[1].offset     = 64;
    skyPushConstantRanges[1].size       = 32; // topColor(12) + pad(4) + bottomColor(12) + useTexture(4)

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount         = 1;
    layoutInfo.pSetLayouts            = &m_TextureDescriptorSetLayout;
    layoutInfo.pushConstantRangeCount = static_cast<uint32_t>(skyPushConstantRanges.size());
    layoutInfo.pPushConstantRanges    = skyPushConstantRanges.data();
    if (vkCreatePipelineLayout(m_Device, &layoutInfo, nullptr, &m_SkyPipelineLayout) != VK_SUCCESS) {
        VE_ENGINE_ERROR("Failed to create sky pipeline layout!");
    }

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount          = 2;
    pipelineInfo.pStages             = shaderStages;
    pipelineInfo.pVertexInputState   = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState      = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState   = &multisampling;
    pipelineInfo.pDepthStencilState  = &depthStencil;
    pipelineInfo.pColorBlendState    = &colorBlending;
    pipelineInfo.pDynamicState       = &dynamicState;
    pipelineInfo.layout              = m_SkyPipelineLayout;
    pipelineInfo.renderPass          = m_RenderPass;
    pipelineInfo.subpass             = 0;

    if (vkCreateGraphicsPipelines(m_Device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_SkyGraphicsPipeline) != VK_SUCCESS) {
        VE_ENGINE_ERROR("Failed to create Vulkan sky pipeline!");
    }

    vkDestroyShaderModule(m_Device, vertModule, nullptr);
    vkDestroyShaderModule(m_Device, fragModule, nullptr);
    VE_ENGINE_INFO("Vulkan sky pipeline created");
}

// ── Framebuffers ────────────────────────────────────────────────────

void VulkanContext::CreateFramebuffers() {
    m_Framebuffers.resize(m_SwapchainImageViews.size());
    for (size_t i = 0; i < m_SwapchainImageViews.size(); i++) {
        std::array<VkImageView, 2> attachments = {
            m_SwapchainImageViews[i],
            m_DepthImageView
        };

        VkFramebufferCreateInfo ci{};
        ci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        ci.renderPass      = m_RenderPass;
        ci.attachmentCount = static_cast<uint32_t>(attachments.size());
        ci.pAttachments    = attachments.data();
        ci.width           = m_SwapchainExtent.width;
        ci.height          = m_SwapchainExtent.height;
        ci.layers          = 1;

        if (vkCreateFramebuffer(m_Device, &ci, nullptr, &m_Framebuffers[i]) != VK_SUCCESS) {
            VE_ENGINE_ERROR("Failed to create framebuffer!");
        }
    }
}

// ── Command pool/buffers ────────────────────────────────────────────

void VulkanContext::CreateCommandPool() {
    VkCommandPoolCreateInfo ci{};
    ci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ci.queueFamilyIndex = static_cast<uint32_t>(m_GraphicsFamily);

    if (vkCreateCommandPool(m_Device, &ci, nullptr, &m_CommandPool) != VK_SUCCESS) {
        VE_ENGINE_ERROR("Failed to create command pool!");
    }
}

void VulkanContext::CreateCommandBuffers() {
    m_CommandBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    VkCommandBufferAllocateInfo ai{};
    ai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool        = m_CommandPool;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = static_cast<uint32_t>(m_CommandBuffers.size());

    if (vkAllocateCommandBuffers(m_Device, &ai, m_CommandBuffers.data()) != VK_SUCCESS) {
        VE_ENGINE_ERROR("Failed to allocate command buffers!");
    }
}

// ── Sync objects ────────────────────────────────────────────────────

void VulkanContext::CreateSyncObjects() {
    m_ImageAvailableSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    m_RenderFinishedSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    m_InFlightFences.resize(MAX_FRAMES_IN_FLIGHT);

    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        vkCreateSemaphore(m_Device, &semInfo, nullptr, &m_ImageAvailableSemaphores[i]);
        vkCreateSemaphore(m_Device, &semInfo, nullptr, &m_RenderFinishedSemaphores[i]);
        vkCreateFence(m_Device, &fenceInfo, nullptr, &m_InFlightFences[i]);
    }
}

// ── Descriptor set infrastructure ────────────────────────────────────

void VulkanContext::CreateDescriptorSetLayout() {
    VkDescriptorSetLayoutBinding samplerBinding{};
    samplerBinding.binding            = 0;
    samplerBinding.descriptorType     = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerBinding.descriptorCount    = 1;
    samplerBinding.stageFlags         = VK_SHADER_STAGE_FRAGMENT_BIT;
    samplerBinding.pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings    = &samplerBinding;

    if (vkCreateDescriptorSetLayout(m_Device, &layoutInfo, nullptr, &m_TextureDescriptorSetLayout) != VK_SUCCESS) {
        VE_ENGINE_ERROR("Failed to create texture descriptor set layout!");
    }
}

void VulkanContext::CreateDescriptorPool() {
    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 64; // support up to 64 textures

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes    = &poolSize;
    poolInfo.maxSets       = 64;

    if (vkCreateDescriptorPool(m_Device, &poolInfo, nullptr, &m_DescriptorPool) != VK_SUCCESS) {
        VE_ENGINE_ERROR("Failed to create descriptor pool!");
    }
}

VkDescriptorSet VulkanContext::AllocateTextureDescriptorSet(VkImageView imageView, VkSampler sampler) {
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_DescriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_TextureDescriptorSetLayout;

    VkDescriptorSet descriptorSet;
    if (vkAllocateDescriptorSets(m_Device, &allocInfo, &descriptorSet) != VK_SUCCESS) {
        VE_ENGINE_ERROR("Failed to allocate texture descriptor set!");
        return VK_NULL_HANDLE;
    }

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView   = imageView;
    imageInfo.sampler     = sampler;

    VkWriteDescriptorSet descriptorWrite{};
    descriptorWrite.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrite.dstSet          = descriptorSet;
    descriptorWrite.dstBinding      = 0;
    descriptorWrite.dstArrayElement = 0;
    descriptorWrite.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrite.descriptorCount = 1;
    descriptorWrite.pImageInfo      = &imageInfo;

    vkUpdateDescriptorSets(m_Device, 1, &descriptorWrite, 0, nullptr);
    return descriptorSet;
}

void VulkanContext::CreateDefaultTexture() {
    // Create a 1x1 white pixel image for use when no texture is bound
    unsigned char whitePixel[4] = { 255, 255, 255, 255 };
    VkDeviceSize imageSize = 4;

    // Staging buffer
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size  = imageSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    vkCreateBuffer(m_Device, &bufferInfo, nullptr, &stagingBuffer);

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(m_Device, stagingBuffer, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize  = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(m_Device, &allocInfo, nullptr, &stagingMemory);
    vkBindBufferMemory(m_Device, stagingBuffer, stagingMemory, 0);

    void* data;
    vkMapMemory(m_Device, stagingMemory, 0, imageSize, 0, &data);
    memcpy(data, whitePixel, 4);
    vkUnmapMemory(m_Device, stagingMemory);

    // Create image
    VkImageCreateInfo imageInfo{};
    imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType     = VK_IMAGE_TYPE_2D;
    imageInfo.format        = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent        = { 1, 1, 1 };
    imageInfo.mipLevels     = 1;
    imageInfo.arrayLayers   = 1;
    imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    vkCreateImage(m_Device, &imageInfo, nullptr, &m_DefaultTexImage);

    VkMemoryRequirements imgMemReqs;
    vkGetImageMemoryRequirements(m_Device, m_DefaultTexImage, &imgMemReqs);

    VkMemoryAllocateInfo imgAllocInfo{};
    imgAllocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    imgAllocInfo.allocationSize  = imgMemReqs.size;
    imgAllocInfo.memoryTypeIndex = FindMemoryType(imgMemReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(m_Device, &imgAllocInfo, nullptr, &m_DefaultTexMemory);
    vkBindImageMemory(m_Device, m_DefaultTexImage, m_DefaultTexMemory, 0);

    // Transition to transfer dst, copy, transition to shader read
    auto beginCmd = [&]() -> VkCommandBuffer {
        VkCommandBufferAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandPool        = m_CommandPool;
        ai.commandBufferCount = 1;
        VkCommandBuffer cb;
        vkAllocateCommandBuffers(m_Device, &ai, &cb);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &bi);
        return cb;
    };
    auto endCmd = [&](VkCommandBuffer cb) {
        vkEndCommandBuffer(cb);
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        vkQueueSubmit(m_GraphicsQueue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_GraphicsQueue);
        vkFreeCommandBuffers(m_Device, m_CommandPool, 1, &cb);
    };

    // Transition UNDEFINED → TRANSFER_DST
    {
        VkCommandBuffer cb = beginCmd();
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_DefaultTexImage;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
        endCmd(cb);
    }

    // Copy buffer to image
    {
        VkCommandBuffer cb = beginCmd();
        VkBufferImageCopy region{};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { 1, 1, 1 };
        vkCmdCopyBufferToImage(cb, stagingBuffer, m_DefaultTexImage,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        endCmd(cb);
    }

    // Transition TRANSFER_DST → SHADER_READ_ONLY
    {
        VkCommandBuffer cb = beginCmd();
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_DefaultTexImage;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
        endCmd(cb);
    }

    vkDestroyBuffer(m_Device, stagingBuffer, nullptr);
    vkFreeMemory(m_Device, stagingMemory, nullptr);

    // Image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image    = m_DefaultTexImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format   = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCreateImageView(m_Device, &viewInfo, nullptr, &m_DefaultTexImageView);

    // Sampler
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType     = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    vkCreateSampler(m_Device, &samplerInfo, nullptr, &m_DefaultTexSampler);

    // Descriptor set
    m_DefaultTextureDS = AllocateTextureDescriptorSet(m_DefaultTexImageView, m_DefaultTexSampler);

    VE_ENGINE_INFO("Vulkan default texture created");
}

// ── Memory helper ───────────────────────────────────────────────────

uint32_t VulkanContext::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(m_PhysicalDevice, &memProps);

    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    }
    VE_ENGINE_ERROR("Failed to find suitable memory type!");
    return 0;
}

// ── Cleanup ─────────────────────────────────────────────────────────

void VulkanContext::CleanupSwapchain() {
    if (m_DepthImageView) { vkDestroyImageView(m_Device, m_DepthImageView, nullptr); m_DepthImageView = VK_NULL_HANDLE; }
    if (m_DepthImage)     { vkDestroyImage(m_Device, m_DepthImage, nullptr);         m_DepthImage = VK_NULL_HANDLE; }
    if (m_DepthImageMemory) { vkFreeMemory(m_Device, m_DepthImageMemory, nullptr);   m_DepthImageMemory = VK_NULL_HANDLE; }

    for (auto fb : m_Framebuffers) vkDestroyFramebuffer(m_Device, fb, nullptr);
    for (auto iv : m_SwapchainImageViews) vkDestroyImageView(m_Device, iv, nullptr);
    if (m_Swapchain) vkDestroySwapchainKHR(m_Device, m_Swapchain, nullptr);
}

void VulkanContext::Cleanup() {
    if (m_Device) vkDeviceWaitIdle(m_Device);

    // Default texture
    if (m_DefaultTexSampler)   vkDestroySampler(m_Device, m_DefaultTexSampler, nullptr);
    if (m_DefaultTexImageView) vkDestroyImageView(m_Device, m_DefaultTexImageView, nullptr);
    if (m_DefaultTexImage)     vkDestroyImage(m_Device, m_DefaultTexImage, nullptr);
    if (m_DefaultTexMemory)    vkFreeMemory(m_Device, m_DefaultTexMemory, nullptr);

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (m_ImageAvailableSemaphores.size() > static_cast<size_t>(i)) {
            vkDestroySemaphore(m_Device, m_ImageAvailableSemaphores[i], nullptr);
            vkDestroySemaphore(m_Device, m_RenderFinishedSemaphores[i], nullptr);
            vkDestroyFence(m_Device, m_InFlightFences[i], nullptr);
        }
    }
    if (m_CommandPool)            vkDestroyCommandPool(m_Device, m_CommandPool, nullptr);
    CleanupSwapchain();
    if (m_DescriptorPool)         vkDestroyDescriptorPool(m_Device, m_DescriptorPool, nullptr);
    if (m_TextureDescriptorSetLayout) vkDestroyDescriptorSetLayout(m_Device, m_TextureDescriptorSetLayout, nullptr);
    if (m_SkyGraphicsPipeline)    vkDestroyPipeline(m_Device, m_SkyGraphicsPipeline, nullptr);
    if (m_SkyPipelineLayout)      vkDestroyPipelineLayout(m_Device, m_SkyPipelineLayout, nullptr);
    if (m_LitGraphicsPipeline)    vkDestroyPipeline(m_Device, m_LitGraphicsPipeline, nullptr);
    if (m_LitPipelineLayout)      vkDestroyPipelineLayout(m_Device, m_LitPipelineLayout, nullptr);
    if (m_GraphicsPipeline)       vkDestroyPipeline(m_Device, m_GraphicsPipeline, nullptr);
    if (m_PipelineLayout)         vkDestroyPipelineLayout(m_Device, m_PipelineLayout, nullptr);
    if (m_RenderPass)             vkDestroyRenderPass(m_Device, m_RenderPass, nullptr);
    if (m_Device)                 vkDestroyDevice(m_Device, nullptr);
    if (m_Surface)                vkDestroySurfaceKHR(m_Instance, m_Surface, nullptr);
    if (m_Instance)               vkDestroyInstance(m_Instance, nullptr);
}

} // namespace VE
