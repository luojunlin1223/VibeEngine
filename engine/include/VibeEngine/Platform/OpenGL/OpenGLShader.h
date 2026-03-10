#pragma once

#include "VibeEngine/Renderer/Shader.h"
#include <cstdint>
#include <unordered_map>

namespace VE {

class OpenGLShader : public Shader {
public:
    OpenGLShader(const std::string& vertexSrc, const std::string& fragmentSrc);
    ~OpenGLShader() override;

    void Bind() const override;
    void Unbind() const override;

    void SetMat4(const std::string& name, const glm::mat4& value) override;
    void SetVec4(const std::string& name, const glm::vec4& value) override;
    void SetVec3(const std::string& name, const glm::vec3& value) override;
    void SetFloat(const std::string& name, float value) override;
    void SetInt(const std::string& name, int value) override;

private:
    int GetUniformLocation(const std::string& name);

    uint32_t m_RendererID = 0;
    mutable std::unordered_map<std::string, int> m_UniformLocationCache;
};

} // namespace VE
