#pragma once

#include "VibeEngine/Renderer/GraphicsContext.h"
#include <vulkan/vulkan.h>
#include <vector>

struct GLFWwindow;

namespace VE {

/*
 * VulkanContext — Initializes Vulkan instance, physical/logical device, and surface.
 * This is the foundation for all Vulkan rendering. Swapchain and rendering pipeline
 * will be built on top of this in future iterations.
 */
class VulkanContext : public GraphicsContext {
public:
    explicit VulkanContext(GLFWwindow* windowHandle);
    ~VulkanContext();

    void Init() override;
    void SwapBuffers() override;

    VkInstance       GetInstance()       const { return m_Instance; }
    VkPhysicalDevice GetPhysicalDevice() const { return m_PhysicalDevice; }
    VkDevice         GetDevice()         const { return m_Device; }
    VkSurfaceKHR     GetSurface()        const { return m_Surface; }

private:
    void CreateInstance();
    void PickPhysicalDevice();
    void CreateLogicalDevice();
    void CreateSurface();
    void Cleanup();

    int FindQueueFamily(VkQueueFlagBits flags);

    GLFWwindow*      m_WindowHandle    = nullptr;
    VkInstance       m_Instance        = VK_NULL_HANDLE;
    VkPhysicalDevice m_PhysicalDevice  = VK_NULL_HANDLE;
    VkDevice         m_Device          = VK_NULL_HANDLE;
    VkSurfaceKHR     m_Surface         = VK_NULL_HANDLE;
    VkQueue          m_GraphicsQueue   = VK_NULL_HANDLE;
    VkQueue          m_PresentQueue    = VK_NULL_HANDLE;
    int              m_GraphicsFamily  = -1;
    int              m_PresentFamily   = -1;
};

} // namespace VE
