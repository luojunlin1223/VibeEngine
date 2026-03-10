#pragma once

#include "VibeEngine/Renderer/GraphicsContext.h"
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector>

struct GLFWwindow;

namespace VE {

constexpr int MAX_FRAMES_IN_FLIGHT = 2;

struct VulkanDrawCommand {
    VkBuffer     VertexBuffer;
    VkBuffer     IndexBuffer;
    uint32_t     IndexCount;
    glm::mat4    MVP;
    glm::mat4    Model;          // for lit pipeline
    bool         UseLitPipeline = false;
};

class VulkanContext : public GraphicsContext {
public:
    explicit VulkanContext(GLFWwindow* windowHandle);
    ~VulkanContext();

    void Init() override;
    void SwapBuffers() override;

    static VulkanContext& Get() { return *s_Instance; }

    VkInstance       GetInstance()       const { return m_Instance; }
    VkPhysicalDevice GetPhysicalDevice() const { return m_PhysicalDevice; }
    VkDevice         GetDevice()         const { return m_Device; }
    VkSurfaceKHR     GetSurface()        const { return m_Surface; }
    VkPipeline       GetPipeline()       const { return m_GraphicsPipeline; }
    VkPipelineLayout GetPipelineLayout() const { return m_PipelineLayout; }
    VkRenderPass     GetRenderPass()     const { return m_RenderPass; }
    VkQueue          GetGraphicsQueue()  const { return m_GraphicsQueue; }
    uint32_t         GetGraphicsFamily() const { return static_cast<uint32_t>(m_GraphicsFamily); }
    uint32_t         GetSwapchainImageCount() const { return static_cast<uint32_t>(m_SwapchainImages.size()); }

    void SetClearColor(float r, float g, float b, float a);
    void SubmitDrawCommand(const VulkanDrawCommand& cmd);

    void SetImGuiEnabled(bool enabled) { m_ImGuiEnabled = enabled; }

    void SetCurrentMVP(const glm::mat4& mvp) { m_CurrentMVP = mvp; }
    const glm::mat4& GetCurrentMVP() const { return m_CurrentMVP; }
    void SetCurrentModel(const glm::mat4& model) { m_CurrentModel = model; }
    const glm::mat4& GetCurrentModel() const { return m_CurrentModel; }
    void SetCurrentUseLit(bool lit) { m_CurrentUseLit = lit; }
    bool GetCurrentUseLit() const { return m_CurrentUseLit; }

    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);

private:
    void CreateInstance();
    void CreateSurface();
    void PickPhysicalDevice();
    void CreateLogicalDevice();
    void CreateSwapchain();
    void CreateImageViews();
    void CreateDepthResources();
    void CreateRenderPass();
    void CreateGraphicsPipeline();
    void CreateLitGraphicsPipeline();
    void CreateFramebuffers();
    void CreateCommandPool();
    void CreateCommandBuffers();
    void CreateSyncObjects();
    void RecreateSwapchain();
    void RecordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex);
    void Cleanup();
    void CleanupSwapchain();

    VkShaderModule CreateShaderModule(const uint32_t* code, size_t size);
    VkFormat FindDepthFormat();

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

    // Depth buffer
    VkImage        m_DepthImage       = VK_NULL_HANDLE;
    VkDeviceMemory m_DepthImageMemory = VK_NULL_HANDLE;
    VkImageView    m_DepthImageView   = VK_NULL_HANDLE;
    VkFormat       m_DepthFormat      = VK_FORMAT_D32_SFLOAT;

    // Unlit pipeline (2D: pos+color, 64-byte push constant MVP)
    VkRenderPass      m_RenderPass       = VK_NULL_HANDLE;
    VkPipelineLayout  m_PipelineLayout   = VK_NULL_HANDLE;
    VkPipeline        m_GraphicsPipeline = VK_NULL_HANDLE;

    // Lit pipeline (3D: pos+normal+color, 128-byte push constant MVP+Model)
    VkPipelineLayout  m_LitPipelineLayout   = VK_NULL_HANDLE;
    VkPipeline        m_LitGraphicsPipeline = VK_NULL_HANDLE;

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
    bool m_ImGuiEnabled = false;
    glm::mat4 m_CurrentMVP   = glm::mat4(1.0f);
    glm::mat4 m_CurrentModel = glm::mat4(1.0f);
    bool m_CurrentUseLit = false;
};

} // namespace VE
