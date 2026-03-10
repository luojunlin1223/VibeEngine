#include "VibeEngine/Platform/Vulkan/VulkanContext.h"
#include "VibeEngine/Core/Log.h"

#include <GLFW/glfw3.h>
#include <vector>
#include <set>

namespace VE {

VulkanContext::VulkanContext(GLFWwindow* windowHandle)
    : m_WindowHandle(windowHandle) {}

VulkanContext::~VulkanContext() {
    Cleanup();
}

void VulkanContext::Init() {
    CreateInstance();
    CreateSurface();
    PickPhysicalDevice();
    CreateLogicalDevice();
    VE_ENGINE_INFO("Vulkan context initialized successfully");
}

void VulkanContext::SwapBuffers() {
    // TODO: Implement swapchain present when swapchain is ready
}

void VulkanContext::CreateInstance() {
    VkApplicationInfo appInfo{};
    appInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName   = "VibeEngine";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName        = "VibeEngine";
    appInfo.engineVersion      = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion         = VK_API_VERSION_1_3;

    // Get required extensions from GLFW
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

    VkResult result = vkCreateInstance(&createInfo, nullptr, &m_Instance);
    if (result != VK_SUCCESS) {
        VE_ENGINE_ERROR("Failed to create Vulkan instance! VkResult: {0}", static_cast<int>(result));
        return;
    }

    VE_ENGINE_INFO("Vulkan instance created");
}

void VulkanContext::CreateSurface() {
    VkResult result = glfwCreateWindowSurface(m_Instance, m_WindowHandle, nullptr, &m_Surface);
    if (result != VK_SUCCESS) {
        VE_ENGINE_ERROR("Failed to create Vulkan window surface!");
        return;
    }
    VE_ENGINE_INFO("Vulkan surface created");
}

void VulkanContext::PickPhysicalDevice() {
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(m_Instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        VE_ENGINE_ERROR("No Vulkan-capable GPU found!");
        return;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(m_Instance, &deviceCount, devices.data());

    // Pick the first suitable device (discrete GPU preferred)
    for (const auto& device : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(device, &props);

        m_PhysicalDevice = device;
        m_GraphicsFamily = FindQueueFamily(VK_QUEUE_GRAPHICS_BIT);

        // Find present family
        for (uint32_t i = 0; ; i++) {
            uint32_t queueFamilyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
            if (i >= queueFamilyCount) break;

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
    std::set<int> uniqueQueueFamilies = { m_GraphicsFamily, m_PresentFamily };
    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;

    float queuePriority = 1.0f;
    for (int family : uniqueQueueFamilies) {
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = static_cast<uint32_t>(family);
        queueCreateInfo.queueCount       = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueCreateInfo);
    }

    VkPhysicalDeviceFeatures deviceFeatures{};

    const char* swapchainExtension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;

    VkDeviceCreateInfo createInfo{};
    createInfo.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount    = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos       = queueCreateInfos.data();
    createInfo.pEnabledFeatures        = &deviceFeatures;
    createInfo.enabledExtensionCount   = 1;
    createInfo.ppEnabledExtensionNames = &swapchainExtension;

    VkResult result = vkCreateDevice(m_PhysicalDevice, &createInfo, nullptr, &m_Device);
    if (result != VK_SUCCESS) {
        VE_ENGINE_ERROR("Failed to create Vulkan logical device!");
        return;
    }

    vkGetDeviceQueue(m_Device, static_cast<uint32_t>(m_GraphicsFamily), 0, &m_GraphicsQueue);
    vkGetDeviceQueue(m_Device, static_cast<uint32_t>(m_PresentFamily),  0, &m_PresentQueue);

    VE_ENGINE_INFO("Vulkan logical device created");
}

int VulkanContext::FindQueueFamily(VkQueueFlagBits flags) {
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_PhysicalDevice, &queueFamilyCount, nullptr);

    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(m_PhysicalDevice, &queueFamilyCount, queueFamilies.data());

    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        if (queueFamilies[i].queueFlags & flags)
            return static_cast<int>(i);
    }
    return -1;
}

void VulkanContext::Cleanup() {
    if (m_Device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_Device);
        vkDestroyDevice(m_Device, nullptr);
    }
    if (m_Surface != VK_NULL_HANDLE)
        vkDestroySurfaceKHR(m_Instance, m_Surface, nullptr);
    if (m_Instance != VK_NULL_HANDLE)
        vkDestroyInstance(m_Instance, nullptr);

    VE_ENGINE_INFO("Vulkan context cleaned up");
}

} // namespace VE
