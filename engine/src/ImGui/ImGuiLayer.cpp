#include "VibeEngine/ImGui/ImGuiLayer.h"
#include "VibeEngine/Core/Log.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_vulkan.h>

#include <GLFW/glfw3.h>

// Vulkan context access (only used when API == Vulkan)
#include "VibeEngine/Platform/Vulkan/VulkanContext.h"

namespace VE {

// ── Vulkan helpers ─────────────────────────────────────────────────

static VkDescriptorPool s_ImGuiDescriptorPool = VK_NULL_HANDLE;

static void CreateVulkanDescriptorPool(VkDevice device) {
    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 },
    };

    VkDescriptorPoolCreateInfo ci{};
    ci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    ci.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    ci.maxSets       = 1;
    ci.poolSizeCount = 1;
    ci.pPoolSizes    = poolSizes;

    vkCreateDescriptorPool(device, &ci, nullptr, &s_ImGuiDescriptorPool);
}

// ── Init / Shutdown ────────────────────────────────────────────────

void ImGuiLayer::Init(GLFWwindow* window, RendererAPI::API api) {
    m_Window = window;
    m_API = api;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // Multi-viewport only for OpenGL (Vulkan viewports need per-window
    // swapchains which we don't support yet)
    if (api == RendererAPI::API::OpenGL)
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    if (api == RendererAPI::API::OpenGL) {
        ImGui_ImplGlfw_InitForOpenGL(m_Window, true);
        ImGui_ImplOpenGL3_Init("#version 460");
    }
    else if (api == RendererAPI::API::Vulkan) {
        auto& ctx = VulkanContext::Get();

        CreateVulkanDescriptorPool(ctx.GetDevice());

        ImGui_ImplGlfw_InitForVulkan(m_Window, true);

        ImGui_ImplVulkan_InitInfo initInfo{};
        initInfo.ApiVersion     = VK_API_VERSION_1_3;
        initInfo.Instance       = ctx.GetInstance();
        initInfo.PhysicalDevice = ctx.GetPhysicalDevice();
        initInfo.Device         = ctx.GetDevice();
        initInfo.QueueFamily    = ctx.GetGraphicsFamily();
        initInfo.Queue          = ctx.GetGraphicsQueue();
        initInfo.DescriptorPool = s_ImGuiDescriptorPool;
        initInfo.MinImageCount  = 2;
        initInfo.ImageCount     = ctx.GetSwapchainImageCount();
        initInfo.PipelineInfoMain.RenderPass  = ctx.GetRenderPass();
        initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

        ImGui_ImplVulkan_Init(&initInfo);

        // Tell VulkanContext to render ImGui draw data
        ctx.SetImGuiEnabled(true);
    }

    VE_ENGINE_INFO("ImGui initialized ({0}, docking{1})",
        api == RendererAPI::API::OpenGL ? "OpenGL" : "Vulkan",
        (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) ? " + viewports" : "");
}

void ImGuiLayer::Shutdown() {
    if (m_API == RendererAPI::API::OpenGL) {
        ImGui_ImplOpenGL3_Shutdown();
    }
    else if (m_API == RendererAPI::API::Vulkan) {
        auto& ctx = VulkanContext::Get();
        vkDeviceWaitIdle(ctx.GetDevice());

        ctx.SetImGuiEnabled(false);
        ImGui_ImplVulkan_Shutdown();

        if (s_ImGuiDescriptorPool) {
            vkDestroyDescriptorPool(ctx.GetDevice(), s_ImGuiDescriptorPool, nullptr);
            s_ImGuiDescriptorPool = VK_NULL_HANDLE;
        }
    }

    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    VE_ENGINE_INFO("ImGui shutdown");
}

// ── Per-frame ──────────────────────────────────────────────────────

void ImGuiLayer::Begin() {
    if (m_API == RendererAPI::API::OpenGL)
        ImGui_ImplOpenGL3_NewFrame();
    else if (m_API == RendererAPI::API::Vulkan)
        ImGui_ImplVulkan_NewFrame();

    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::End() {
    ImGui::Render();

    if (m_API == RendererAPI::API::OpenGL) {
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        ImGuiIO& io = ImGui::GetIO();
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            GLFWwindow* backupContext = glfwGetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            glfwMakeContextCurrent(backupContext);
        }
    }
    // For Vulkan, the draw data will be consumed by
    // VulkanContext::RecordCommandBuffer() during SwapBuffers().
}

} // namespace VE
