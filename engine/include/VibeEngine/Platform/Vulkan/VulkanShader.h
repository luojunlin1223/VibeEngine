#pragma once

#include "VibeEngine/Renderer/Shader.h"
#include "VibeEngine/Platform/Vulkan/VulkanContext.h"

namespace VE {

// Vulkan shaders are managed by the pipeline in VulkanContext.
// Uniform setters forward relevant data to VulkanContext for push constants.
class VulkanShader : public Shader {
public:
    VulkanShader(const std::string& /*vertexSrc*/, const std::string& /*fragmentSrc*/) {}
    void Bind() const override {}
    void Unbind() const override {}

    void SetMat4(const std::string& name, const glm::mat4& value) override {
        if (name == "u_MVP")
            VulkanContext::Get().SetCurrentMVP(value);
        else if (name == "u_Model") {
            VulkanContext::Get().SetCurrentModel(value);
            VulkanContext::Get().SetCurrentUseLit(true);
        }
    }

    void SetVec4(const std::string& name, const glm::vec4& value) override {
        if (name == "u_EntityColor")
            VulkanContext::Get().SetCurrentEntityColor(value);
    }
    void SetVec3(const std::string& name, const glm::vec3& value) override {
        if (name == "u_LightDir")
            VulkanContext::Get().SetCurrentLightDir(value);
        else if (name == "u_LightColor")
            VulkanContext::Get().SetCurrentLightColor(value);
        else if (name == "u_ViewPos")
            VulkanContext::Get().SetCurrentViewPos(value);
        else if (name == "u_TopColor") {
            VulkanContext::Get().SetCurrentSkyTopColor(value);
            VulkanContext::Get().SetCurrentUseSky(true);
        }
        else if (name == "u_BottomColor")
            VulkanContext::Get().SetCurrentSkyBottomColor(value);
    }
    void SetFloat(const std::string& name, float value) override {
        if (name == "u_LightIntensity")
            VulkanContext::Get().SetCurrentLightIntensity(value);
    }

    void SetInt(const std::string& name, int value) override {
        if (name == "u_UseTexture")
            VulkanContext::Get().SetCurrentUseTexture(value);
        // u_Texture (sampler slot) is handled via descriptor set binding
    }
};

} // namespace VE
