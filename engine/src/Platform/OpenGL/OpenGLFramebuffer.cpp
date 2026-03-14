#include "VibeEngine/Platform/OpenGL/OpenGLFramebuffer.h"
#include "VibeEngine/Core/Log.h"
#include <glad/gl.h>

namespace VE {

OpenGLFramebuffer::OpenGLFramebuffer(const FramebufferSpec& spec)
    : m_Width(spec.Width), m_Height(spec.Height), m_HDR(spec.HDR),
      m_Samples(spec.Samples), m_Multisampled(spec.Samples > 1)
{
    Invalidate();
}

OpenGLFramebuffer::~OpenGLFramebuffer() {
    Cleanup();
}

void OpenGLFramebuffer::Invalidate() {
    Cleanup();

    GLenum internalFormat = m_HDR ? GL_RGBA16F : GL_RGBA8;

    glGenFramebuffers(1, &m_FBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);

    if (m_Multisampled) {
        // ── Multisample color attachment ──
        glGenTextures(1, &m_ColorAttachment);
        glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, m_ColorAttachment);
        glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, m_Samples, internalFormat,
                                m_Width, m_Height, GL_TRUE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D_MULTISAMPLE, m_ColorAttachment, 0);

        // ── Multisample depth texture ──
        glGenTextures(1, &m_DepthAttachment);
        glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, m_DepthAttachment);
        glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, m_Samples,
                                GL_DEPTH24_STENCIL8, m_Width, m_Height, GL_TRUE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                               GL_TEXTURE_2D_MULTISAMPLE, m_DepthAttachment, 0);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            VE_ENGINE_ERROR("MSAA Framebuffer is not complete!");

        // ── Resolve FBO (non-multisample) ──
        glGenFramebuffers(1, &m_ResolveFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, m_ResolveFBO);

        // Resolve color
        glGenTextures(1, &m_ResolveColorAttachment);
        glBindTexture(GL_TEXTURE_2D, m_ResolveColorAttachment);
        if (m_HDR)
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, m_Width, m_Height, 0, GL_RGBA, GL_FLOAT, nullptr);
        else
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_Width, m_Height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, m_ResolveColorAttachment, 0);

        // Resolve depth (for SSAO sampling)
        glGenTextures(1, &m_ResolveDepthAttachment);
        glBindTexture(GL_TEXTURE_2D, m_ResolveDepthAttachment);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, m_Width, m_Height, 0,
                     GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                               GL_TEXTURE_2D, m_ResolveDepthAttachment, 0);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            VE_ENGINE_ERROR("MSAA Resolve Framebuffer is not complete!");
    } else {
        // ── Standard color texture ──
        glGenTextures(1, &m_ColorAttachment);
        glBindTexture(GL_TEXTURE_2D, m_ColorAttachment);
        if (m_HDR)
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, m_Width, m_Height, 0, GL_RGBA, GL_FLOAT, nullptr);
        else
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_Width, m_Height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, m_ColorAttachment, 0);

        // ── Depth texture (sampleable for SSAO) ──
        glGenTextures(1, &m_DepthAttachment);
        glBindTexture(GL_TEXTURE_2D, m_DepthAttachment);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, m_Width, m_Height, 0,
                     GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                               GL_TEXTURE_2D, m_DepthAttachment, 0);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            VE_ENGINE_ERROR("Framebuffer is not complete!");
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void OpenGLFramebuffer::Cleanup() {
    if (m_FBO) {
        glDeleteFramebuffers(1, &m_FBO);
        glDeleteTextures(1, &m_ColorAttachment);
        glDeleteTextures(1, &m_DepthAttachment);
        m_FBO = 0;
        m_ColorAttachment = 0;
        m_DepthAttachment = 0;
    }
    if (m_ResolveFBO) {
        glDeleteFramebuffers(1, &m_ResolveFBO);
        glDeleteTextures(1, &m_ResolveColorAttachment);
        glDeleteTextures(1, &m_ResolveDepthAttachment);
        m_ResolveFBO = 0;
        m_ResolveColorAttachment = 0;
        m_ResolveDepthAttachment = 0;
    }
}

void OpenGLFramebuffer::Bind() {
    glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
    glViewport(0, 0, m_Width, m_Height);
}

void OpenGLFramebuffer::Unbind() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

uint64_t OpenGLFramebuffer::Resolve() {
    if (!m_Multisampled)
        return static_cast<uint64_t>(m_ColorAttachment);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_FBO);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_ResolveFBO);
    glBlitFramebuffer(0, 0, m_Width, m_Height,
                      0, 0, m_Width, m_Height,
                      GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    return static_cast<uint64_t>(m_ResolveColorAttachment);
}

void OpenGLFramebuffer::Resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0 || width > 8192 || height > 8192) return;
    if (width == m_Width && height == m_Height) return;
    m_Width = width;
    m_Height = height;
    Invalidate();
}

} // namespace VE
