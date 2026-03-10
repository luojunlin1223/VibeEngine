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
    }

    void SetVec4(const std::string&, const glm::vec4&) override {}
    void SetVec3(const std::string&, const glm::vec3&) override {}
    void SetFloat(const std::string&, float) override {}
    void SetInt(const std::string&, int) override {}
};

} // namespace VE
