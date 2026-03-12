/*
 * ImGuiLayer — Manages Dear ImGui lifecycle within the engine.
 *
 * Supports both OpenGL and Vulkan backends. The backend is chosen
 * based on RendererAPI::GetAPI() at Init() time.
 *
 * For Vulkan, the actual draw data rendering happens inside
 * VulkanContext::RecordCommandBuffer(). ImGuiLayer::End() only
 * calls ImGui::Render() to prepare the draw data.
 */
#pragma once

#include "VibeEngine/Renderer/RendererAPI.h"

struct GLFWwindow;

namespace VE {

class ImGuiLayer {
public:
    ImGuiLayer() = default;
    ~ImGuiLayer() = default;

    void Init(GLFWwindow* window, RendererAPI::API api);
    void Shutdown();

    void Begin();
    void End();

private:
    GLFWwindow* m_Window = nullptr;
    RendererAPI::API m_API = RendererAPI::API::None;
};

} // namespace VE
