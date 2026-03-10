#pragma once

#include "VibeEngine/Renderer/Shader.h"
#include <cstdint>

namespace VE {

class OpenGLShader : public Shader {
public:
    OpenGLShader(const std::string& vertexSrc, const std::string& fragmentSrc);
    ~OpenGLShader() override;

    void Bind() const override;
    void Unbind() const override;

private:
    uint32_t m_RendererID = 0;
};

} // namespace VE
