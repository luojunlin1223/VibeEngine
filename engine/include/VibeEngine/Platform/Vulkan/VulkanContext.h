#pragma once

#include "VibeEngine/Renderer/GraphicsContext.h"
#include <vulkan/vulkan.h>
#include <vector>

struct GLFWwindow;

namespace VE {

constexpr int MAX_FRAMES_IN_FLIGHT = 2;

struct VulkanDrawCommand {
    VkBuffer     VertexBuffer;
    VkBuffer     IndexBuffer;
    uint32_t     IndexCount;
};

class VulkanContext : public GraphicsContext {
public:
    explicit VulkanContext(GLFWwindow* windowHandle);
    ~VulkanContext();

    void Init() override;
    void SwapBuffers() override;

    // Singleton accessor for Vulkan platform classes
    static VulkanContext& Get() { return *s_Instance; }

    VkInstance       GetInstance()       const { return m_Instance; }
    VkPhysicalDevice GetPhysicalDevice() const { return m_PhysicalDevice; }
    VkDevice         GetDevice()         const { return m_Device; }
    VkSurfaceKHR     GetSurface()        const { return m_Surface; }
    VkPipeline       GetPipeline()       const { return m_GraphicsPipeline; }
    VkPipelineLayout GetPipelineLayout() const { return m_PipelineLayout; }

    void SetClearColor(float r, float g, float b, float a);
    void SubmitDrawCommand(const VulkanDrawCommand& cmd);

    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);

private:
    void CreateInstance();
    void CreateSurface();
    void PickPhysicalDevice();
    void CreateLogicalDevice();
    void CreateSwapchain();
    void CreateImageViews();
    void CreateRenderPass();
    void CreateGraphicsPipeline();
    void CreateFramebuffers();
    void CreateCommandPool();
    void CreateCommandBuffers();
    void CreateSyncObjects();
    void RecordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex);
    void Cleanup();
    void CleanupSwapchain();

    VkShaderModule CreateShaderModule(const uint32_t* code, size_t size);

    int FindQueueFamily(VkQueueFlagBits flags);

    static VulkanContext* s_Instance;

    GLFWwindow*       m_WindowHandle     = nullptr;
    VkInstance        m_Instance         = VK_NULL_HANDLE;
    VkPhysicalDevice  m_PhysicalDevice   = VK_NULL_HANDLE;
    VkDevice          m_Device           = VK_NULL_HANDLE;
    VkSurfaceKHR      m_Surface          = VK_NULL_HANDLE;
    VkQueue           m_GraphicsQueue    = VK_NULL_HANDLE;
    VkQueue           m_PresentQueue     = VK_NULL_HANDLE;
    int               m_GraphicsFamily   = -1;
    int               m_PresentFamily    = -1;

    // Swapchain
    VkSwapchainKHR             m_Swapchain       = VK_NULL_HANDLE;
    std::vector<VkImage>       m_SwapchainImages;
    std::vector<VkImageView>   m_SwapchainImageViews;
    VkFormat                   m_SwapchainFormat  = VK_FORMAT_UNDEFINED;
    VkExtent2D                 m_SwapchainExtent  = {0, 0};

    // Pipeline
    VkRenderPass      m_RenderPass       = VK_NULL_HANDLE;
    VkPipelineLayout  m_PipelineLayout   = VK_NULL_HANDLE;
    VkPipeline        m_GraphicsPipeline = VK_NULL_HANDLE;

    // Framebuffers
    std::vector<VkFramebuffer> m_Framebuffers;

    // Command buffers
    VkCommandPool                m_CommandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> m_CommandBuffers;

    // Sync
    std::vector<VkSemaphore> m_ImageAvailableSemaphores;
    std::vector<VkSemaphore> m_RenderFinishedSemaphores;
    std::vector<VkFence>     m_InFlightFences;
    uint32_t                 m_CurrentFrame = 0;

    // Render state
    VkClearColorValue m_ClearColor = {{0.1f, 0.1f, 0.1f, 1.0f}};
    std::vector<VulkanDrawCommand> m_DrawCommands;
};

} // namespace VE
