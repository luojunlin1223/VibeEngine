#pragma once

#include "VibeEngine/Renderer/ComputeShader.h"

#include <cstdint>
#include <unordered_map>

namespace VE {

class OpenGLComputeShader : public ComputeShader {
public:
    explicit OpenGLComputeShader(const std::string& computeSrc);
    ~OpenGLComputeShader() override;

    void Bind() const override;
    void Unbind() const override;
    void Dispatch(uint32_t groupX, uint32_t groupY, uint32_t groupZ) const override;
    void MemoryBarrier(uint32_t barrierBits) const override;

    void SetMat4(const std::string& name, const glm::mat4& value) override;
    void SetVec3(const std::string& name, const glm::vec3& value) override;
    void SetFloat(const std::string& name, float value) override;
    void SetInt(const std::string& name, int value) override;

private:
    int GetUniformLocation(const std::string& name);

    uint32_t m_RendererID = 0;
    mutable std::unordered_map<std::string, int> m_UniformLocationCache;
};

} // namespace VE
