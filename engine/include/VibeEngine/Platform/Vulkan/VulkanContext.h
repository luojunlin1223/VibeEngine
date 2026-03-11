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
    VkDescriptorSet TextureDescriptorSet = VK_NULL_HANDLE;
    int          UseTexture = 0;
    glm::vec3    LightDir      = glm::vec3(0.3f, 1.0f, 0.5f);
    glm::vec3    LightColor    = glm::vec3(1.0f);
    float        LightIntensity = 1.0f;
    glm::vec3    ViewPos       = glm::vec3(0.0f);
    glm::vec4    EntityColor   = glm::vec4(1.0f);
    float        Metallic      = 0.0f;
    float        Roughness     = 0.5f;
    float        AO            = 1.0f;
    bool         UseSkyPipeline = false;
    glm::vec3    SkyTopColor   = glm::vec3(0.4f, 0.7f, 1.0f);
    glm::vec3    SkyBottomColor = glm::vec3(0.9f, 0.9f, 0.95f);
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

    void SetCurrentTextureDescriptorSet(VkDescriptorSet ds) { m_CurrentTextureDS = ds; }
    VkDescriptorSet GetCurrentTextureDescriptorSet() const { return m_CurrentTextureDS; }
    void SetCurrentUseTexture(int use) { m_CurrentUseTexture = use; }
    int  GetCurrentUseTexture() const { return m_CurrentUseTexture; }

    void SetCurrentLightDir(const glm::vec3& v) { m_CurrentLightDir = v; }
    const glm::vec3& GetCurrentLightDir() const { return m_CurrentLightDir; }
    void SetCurrentLightColor(const glm::vec3& v) { m_CurrentLightColor = v; }
    const glm::vec3& GetCurrentLightColor() const { return m_CurrentLightColor; }
    void SetCurrentLightIntensity(float v) { m_CurrentLightIntensity = v; }
    float GetCurrentLightIntensity() const { return m_CurrentLightIntensity; }
    void SetCurrentViewPos(const glm::vec3& v) { m_CurrentViewPos = v; }
    const glm::vec3& GetCurrentViewPos() const { return m_CurrentViewPos; }
    void SetCurrentEntityColor(const glm::vec4& v) { m_CurrentEntityColor = v; }
    const glm::vec4& GetCurrentEntityColor() const { return m_CurrentEntityColor; }

    void SetCurrentMetallic(float v) { m_CurrentMetallic = v; }
    float GetCurrentMetallic() const { return m_CurrentMetallic; }
    void SetCurrentRoughness(float v) { m_CurrentRoughness = v; }
    float GetCurrentRoughness() const { return m_CurrentRoughness; }
    void SetCurrentAO(float v) { m_CurrentAO = v; }
    float GetCurrentAO() const { return m_CurrentAO; }

    void SetCurrentUseSky(bool v) { m_CurrentUseSky = v; }
    bool GetCurrentUseSky() const { return m_CurrentUseSky; }
    void SetCurrentSkyTopColor(const glm::vec3& v) { m_CurrentSkyTopColor = v; }
    const glm::vec3& GetCurrentSkyTopColor() const { return m_CurrentSkyTopColor; }
    void SetCurrentSkyBottomColor(const glm::vec3& v) { m_CurrentSkyBottomColor = v; }
    const glm::vec3& GetCurrentSkyBottomColor() const { return m_CurrentSkyBottomColor; }

    VkCommandPool GetCommandPool() const { return m_CommandPool; }
    VkDescriptorSet GetDefaultTextureDescriptorSet() const { return m_DefaultTextureDS; }

    // Allocate a descriptor set for a texture (image + sampler)
    VkDescriptorSet AllocateTextureDescriptorSet(VkImageView imageView, VkSampler sampler);

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
    void CreateDescriptorSetLayout();
    void CreateDescriptorPool();
    void CreateDefaultTexture();
    void CreateGraphicsPipeline();
    void CreateLitGraphicsPipeline();
    void CreateSkyGraphicsPipeline();
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

    // Sky pipeline (depth write off, LEQUAL, cull front)
    VkPipelineLayout  m_SkyPipelineLayout   = VK_NULL_HANDLE;
    VkPipeline        m_SkyGraphicsPipeline = VK_NULL_HANDLE;

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

    // Descriptor set infrastructure
    VkDescriptorSetLayout m_TextureDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_DescriptorPool             = VK_NULL_HANDLE;

    // Default 1x1 white texture (bound when no texture is assigned)
    VkImage        m_DefaultTexImage       = VK_NULL_HANDLE;
    VkDeviceMemory m_DefaultTexMemory      = VK_NULL_HANDLE;
    VkImageView    m_DefaultTexImageView   = VK_NULL_HANDLE;
    VkSampler      m_DefaultTexSampler     = VK_NULL_HANDLE;
    VkDescriptorSet m_DefaultTextureDS     = VK_NULL_HANDLE;

    // Render state
    VkClearColorValue m_ClearColor = {{0.1f, 0.1f, 0.1f, 1.0f}};
    std::vector<VulkanDrawCommand> m_DrawCommands;
    bool m_ImGuiEnabled = false;
    glm::mat4 m_CurrentMVP   = glm::mat4(1.0f);
    glm::mat4 m_CurrentModel = glm::mat4(1.0f);
    bool m_CurrentUseLit = false;
    VkDescriptorSet m_CurrentTextureDS = VK_NULL_HANDLE;
    int  m_CurrentUseTexture = 0;
    bool      m_CurrentUseSky        = false;
    glm::vec3 m_CurrentSkyTopColor   = glm::vec3(0.4f, 0.7f, 1.0f);
    glm::vec3 m_CurrentSkyBottomColor = glm::vec3(0.9f, 0.9f, 0.95f);
    glm::vec3 m_CurrentLightDir      = glm::vec3(0.3f, 1.0f, 0.5f);
    glm::vec3 m_CurrentLightColor    = glm::vec3(1.0f);
    float     m_CurrentLightIntensity = 1.0f;
    glm::vec3 m_CurrentViewPos       = glm::vec3(0.0f);
    glm::vec4 m_CurrentEntityColor   = glm::vec4(1.0f);
    float     m_CurrentMetallic      = 0.0f;
    float     m_CurrentRoughness     = 0.5f;
    float     m_CurrentAO            = 1.0f;
};

} // namespace VE
