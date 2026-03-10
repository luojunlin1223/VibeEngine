#include "VibeEngine/Platform/OpenGL/OpenGLShader.h"
#include "VibeEngine/Core/Log.h"

#include <glad/gl.h>
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
}

OpenGLShader::~OpenGLShader() {
    glDeleteProgram(m_RendererID);
}

void OpenGLShader::Bind() const {
    glUseProgram(m_RendererID);
}

void OpenGLShader::Unbind() const {
    glUseProgram(0);
}

} // namespace VE
