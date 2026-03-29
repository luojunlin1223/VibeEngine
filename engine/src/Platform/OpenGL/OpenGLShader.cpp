#include "VibeEngine/Platform/OpenGL/OpenGLShader.h"
#include "VibeEngine/Core/Log.h"
#include "VibeEngine/Renderer/GPUResourceTracker.h"

#include <glad/gl.h>
#include <glm/gtc/type_ptr.hpp>
#include <vector>

namespace VE {

static GLuint CompileShader(GLenum type, const std::string& source) {
    GLuint shader = glCreateShader(type);
    const char* src = source.c_str();
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLint length;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        std::vector<char> log(static_cast<size_t>(length));
        glGetShaderInfoLog(shader, length, &length, log.data());
        VE_ENGINE_ERROR("Shader compilation failed: {0}", log.data());
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

OpenGLShader::OpenGLShader(const std::string& vertexSrc, const std::string& fragmentSrc) {
    GLuint vertexShader   = CompileShader(GL_VERTEX_SHADER, vertexSrc);
    GLuint fragmentShader = CompileShader(GL_FRAGMENT_SHADER, fragmentSrc);

    if (!vertexShader || !fragmentShader) {
        VE_ENGINE_ERROR("Failed to compile shaders");
        return;
    }

    m_RendererID = glCreateProgram();
    glAttachShader(m_RendererID, vertexShader);
    glAttachShader(m_RendererID, fragmentShader);
    glLinkProgram(m_RendererID);

    GLint success;
    glGetProgramiv(m_RendererID, GL_LINK_STATUS, &success);
    if (!success) {
        GLint length;
        glGetProgramiv(m_RendererID, GL_INFO_LOG_LENGTH, &length);
        std::vector<char> log(static_cast<size_t>(length));
        glGetProgramInfoLog(m_RendererID, length, &length, log.data());
        VE_ENGINE_ERROR("Shader link failed: {0}", log.data());
    }

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    // Validate program
    glValidateProgram(m_RendererID);
    GLint valid;
    glGetProgramiv(m_RendererID, GL_VALIDATE_STATUS, &valid);
    if (!valid) {
        GLint length;
        glGetProgramiv(m_RendererID, GL_INFO_LOG_LENGTH, &length);
        std::vector<char> log(static_cast<size_t>(length));
        glGetProgramInfoLog(m_RendererID, length, &length, log.data());
        VE_ENGINE_WARN("Shader program validation warning: {0}", log.data());
    }

    VE_GPU_TRACK(GPUResourceType::ShaderProgram, m_RendererID);
}

OpenGLShader::~OpenGLShader() {
    VE_GPU_UNTRACK(GPUResourceType::ShaderProgram, m_RendererID);
    glDeleteProgram(m_RendererID);
}

void OpenGLShader::Bind() const {
    glUseProgram(m_RendererID);
}

void OpenGLShader::Unbind() const {
    glUseProgram(0);
}

int OpenGLShader::GetUniformLocation(const std::string& name) {
    auto it = m_UniformLocationCache.find(name);
    if (it != m_UniformLocationCache.end())
        return it->second;

    int location = glGetUniformLocation(m_RendererID, name.c_str());
    m_UniformLocationCache[name] = location;
    return location;
}

void OpenGLShader::SetMat4(const std::string& name, const glm::mat4& value) {
    glUseProgram(m_RendererID);
    glUniformMatrix4fv(GetUniformLocation(name), 1, GL_FALSE, glm::value_ptr(value));
}


void OpenGLShader::SetVec4(const std::string& name, const glm::vec4& value) {
    glUseProgram(m_RendererID);
    glUniform4fv(GetUniformLocation(name), 1, glm::value_ptr(value));
}

void OpenGLShader::SetVec3(const std::string& name, const glm::vec3& value) {
    glUseProgram(m_RendererID);
    glUniform3fv(GetUniformLocation(name), 1, glm::value_ptr(value));
}

void OpenGLShader::SetFloat(const std::string& name, float value) {
    glUseProgram(m_RendererID);
    glUniform1f(GetUniformLocation(name), value);
}

void OpenGLShader::SetInt(const std::string& name, int value) {
    glUseProgram(m_RendererID);
    glUniform1i(GetUniformLocation(name), value);
}

} // namespace VE
