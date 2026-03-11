#include "VibeEngine/Platform/OpenGL/OpenGLTexture.h"
#include "VibeEngine/Core/Log.h"

#include <glad/gl.h>
#include <stb_image.h>

namespace VE {

OpenGLTexture2D::OpenGLTexture2D(const std::string& path)
    : m_FilePath(path)
{
    stbi_set_flip_vertically_on_load(true);

    int width, height, channels;
    unsigned char* data = stbi_load(path.c_str(), &width, &height, &channels, 0);
    if (!data) {
        VE_ENGINE_ERROR("Failed to load texture: {0}", path);
        return;
    }

    m_Width  = static_cast<uint32_t>(width);
    m_Height = static_cast<uint32_t>(height);

    GLenum internalFormat = GL_RGB8;
    GLenum dataFormat     = GL_RGB;
    if (channels == 4) {
        internalFormat = GL_RGBA8;
        dataFormat     = GL_RGBA;
    } else if (channels == 1) {
        internalFormat = GL_R8;
        dataFormat     = GL_RED;
    }

    glGenTextures(1, &m_RendererID);
    glBindTexture(GL_TEXTURE_2D, m_RendererID);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internalFormat),
                 width, height, 0, dataFormat, GL_UNSIGNED_BYTE, data);
    glGenerateMipmap(GL_TEXTURE_2D);

    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(data);

    VE_ENGINE_INFO("Texture loaded: {0} ({1}x{2}, {3}ch) [GL ID={4}]", path, width, height, channels, m_RendererID);
}

OpenGLTexture2D::~OpenGLTexture2D() {
    glDeleteTextures(1, &m_RendererID);
}

void OpenGLTexture2D::Bind(uint32_t slot) const {
    glActiveTexture(GL_TEXTURE0 + slot);
    glBindTexture(GL_TEXTURE_2D, m_RendererID);
}

void OpenGLTexture2D::Unbind() const {
    glBindTexture(GL_TEXTURE_2D, 0);
}

} // namespace VE
