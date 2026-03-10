#pragma once

#include "VibeEngine/Renderer/Shader.h"

namespace VE {

// Vulkan shaders are managed by the pipeline in VulkanContext.
// This is a stub to satisfy the abstraction layer.
class VulkanShader : public Shader {
public:
    VulkanShader(const std::string& /*vertexSrc*/, const std::string& /*fragmentSrc*/) {}
    void Bind() const override {}
    void Unbind() const override {}
};

} // namespace VE
