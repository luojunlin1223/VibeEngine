#include "VibeEngine/Platform/OpenGL/OpenGLComputeShader.h"
#include "VibeEngine/Core/Log.h"
#include "VibeEngine/Renderer/GPUResourceTracker.h"

#include <glad/gl.h>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <vector>

namespace VE {

namespace {

GLuint CompileComputeShader(const std::string& source) {
    GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
    const char* src = source.c_str();
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    GLint success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLint length = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        std::vector<char> log(static_cast<size_t>(std::max(length, 1)));
        glGetShaderInfoLog(shader, length, &length, log.data());
        VE_ENGINE_ERROR("Compute shader compilation failed: {0}", log.data());
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

}

OpenGLComputeShader::OpenGLComputeShader(const std::string& computeSrc) {
    GLuint computeShader = CompileComputeShader(computeSrc);
    if (!computeShader) {
        VE_ENGINE_ERROR("Failed to compile compute shader");
        return;
    }

    m_RendererID = glCreateProgram();
    glAttachShader(m_RendererID, computeShader);
    glLinkProgram(m_RendererID);

    GLint success = 0;
    glGetProgramiv(m_RendererID, GL_LINK_STATUS, &success);
    if (!success) {
        GLint length = 0;
        glGetProgramiv(m_RendererID, GL_INFO_LOG_LENGTH, &length);
        std::vector<char> log(static_cast<size_t>(std::max(length, 1)));
        glGetProgramInfoLog(m_RendererID, length, &length, log.data());
        VE_ENGINE_ERROR("Compute shader link failed: {0}", log.data());
    }

    glDeleteShader(computeShader);

    if (m_RendererID != 0)
        VE_GPU_TRACK(GPUResourceType::ShaderProgram, m_RendererID);
}

OpenGLComputeShader::~OpenGLComputeShader() {
    VE_GPU_UNTRACK(GPUResourceType::ShaderProgram, m_RendererID);
    if (m_RendererID != 0)
        glDeleteProgram(m_RendererID);
}

void OpenGLComputeShader::Bind() const {
    glUseProgram(m_RendererID);
}

void OpenGLComputeShader::Unbind() const {
    glUseProgram(0);
}

void OpenGLComputeShader::Dispatch(uint32_t groupX, uint32_t groupY, uint32_t groupZ) const {
    glUseProgram(m_RendererID);
    glDispatchCompute(static_cast<GLuint>(groupX),
                      static_cast<GLuint>(groupY),
                      static_cast<GLuint>(groupZ));
}

void OpenGLComputeShader::MemoryBarrier(uint32_t barrierBits) const {
    glMemoryBarrier(static_cast<GLbitfield>(barrierBits));
}

int OpenGLComputeShader::GetUniformLocation(const std::string& name) {
    auto it = m_UniformLocationCache.find(name);
    if (it != m_UniformLocationCache.end())
        return it->second;

    int location = glGetUniformLocation(m_RendererID, name.c_str());
    m_UniformLocationCache[name] = location;
    return location;
}

void OpenGLComputeShader::SetMat4(const std::string& name, const glm::mat4& value) {
    glUseProgram(m_RendererID);
    glUniformMatrix4fv(GetUniformLocation(name), 1, GL_FALSE, glm::value_ptr(value));
}

void OpenGLComputeShader::SetVec3(const std::string& name, const glm::vec3& value) {
    glUseProgram(m_RendererID);
    glUniform3fv(GetUniformLocation(name), 1, glm::value_ptr(value));
}

void OpenGLComputeShader::SetFloat(const std::string& name, float value) {
    glUseProgram(m_RendererID);
    glUniform1f(GetUniformLocation(name), value);
}

void OpenGLComputeShader::SetInt(const std::string& name, int value) {
    glUseProgram(m_RendererID);
    glUniform1i(GetUniformLocation(name), value);
}

} // namespace VE
