#include "VibeEngine/Platform/OpenGL/OpenGLFramebuffer.h"
#include "VibeEngine/Core/Log.h"
#include "VibeEngine/Renderer/GPUResourceTracker.h"
#include <glad/gl.h>

namespace VE {

OpenGLFramebuffer::OpenGLFramebuffer(const FramebufferSpec& spec)
    : m_Width(spec.Width), m_Height(spec.Height), m_HDR(spec.HDR),
      m_Samples(spec.Samples), m_Multisampled(spec.Samples > 1),
      m_MRT(!spec.ColorFormats.empty()), m_ColorFormats(spec.ColorFormats)
{
    Invalidate();
}

OpenGLFramebuffer::~OpenGLFramebuffer() {
    Cleanup();
}

void OpenGLFramebuffer::Invalidate() {
    Cleanup();

    glGenFramebuffers(1, &m_FBO);
    VE_GPU_TRACK(GPUResourceType::Framebuffer, m_FBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);

    // ── MRT path: multiple color attachments ──
    if (m_MRT) {
        int numAttachments = static_cast<int>(m_ColorFormats.size());
        m_ColorAttachments.resize(numAttachments);
        glGenTextures(numAttachments, m_ColorAttachments.data());

        std::vector<GLenum> drawBuffers(numAttachments);
        for (int i = 0; i < numAttachments; ++i) {
            VE_GPU_TRACK(GPUResourceType::Texture, m_ColorAttachments[i]);
            glBindTexture(GL_TEXTURE_2D, m_ColorAttachments[i]);

            GLenum internalFmt = m_ColorFormats[i].InternalFormat;
            // Determine format and type from internal format
            GLenum format = GL_RGBA;
            GLenum type = GL_UNSIGNED_BYTE;
            if (internalFmt == GL_RGBA16F || internalFmt == GL_RGBA32F) {
                type = GL_FLOAT;
            }

            glTexImage2D(GL_TEXTURE_2D, 0, internalFmt, m_Width, m_Height, 0,
                         format, type, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i,
                                   GL_TEXTURE_2D, m_ColorAttachments[i], 0);
            drawBuffers[i] = GL_COLOR_ATTACHMENT0 + i;
        }
        glDrawBuffers(numAttachments, drawBuffers.data());

        // Depth texture (always created for MRT)
        glGenTextures(1, &m_DepthAttachment);
        VE_GPU_TRACK(GPUResourceType::Texture, m_DepthAttachment);
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
            VE_ENGINE_ERROR("MRT Framebuffer is not complete!");

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }

    // ── Standard (non-MRT) path ──
    GLenum internalFormat = m_HDR ? GL_RGBA16F : GL_RGBA8;

    if (m_Multisampled) {
        // ── Multisample color attachment ──
        glGenTextures(1, &m_ColorAttachment);
        VE_GPU_TRACK(GPUResourceType::Texture, m_ColorAttachment);
        glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, m_ColorAttachment);
        glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, m_Samples, internalFormat,
                                m_Width, m_Height, GL_TRUE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D_MULTISAMPLE, m_ColorAttachment, 0);

        // ── Multisample depth texture ──
        glGenTextures(1, &m_DepthAttachment);
        VE_GPU_TRACK(GPUResourceType::Texture, m_DepthAttachment);
        glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, m_DepthAttachment);
        glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, m_Samples,
                                GL_DEPTH24_STENCIL8, m_Width, m_Height, GL_TRUE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                               GL_TEXTURE_2D_MULTISAMPLE, m_DepthAttachment, 0);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            VE_ENGINE_ERROR("MSAA Framebuffer is not complete!");

        // ── Resolve FBO (non-multisample) ──
        glGenFramebuffers(1, &m_ResolveFBO);
        VE_GPU_TRACK(GPUResourceType::Framebuffer, m_ResolveFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, m_ResolveFBO);

        // Resolve color
        glGenTextures(1, &m_ResolveColorAttachment);
        VE_GPU_TRACK(GPUResourceType::Texture, m_ResolveColorAttachment);
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
        VE_GPU_TRACK(GPUResourceType::Texture, m_ResolveDepthAttachment);
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
        VE_GPU_TRACK(GPUResourceType::Texture, m_ColorAttachment);
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
        VE_GPU_TRACK(GPUResourceType::Texture, m_DepthAttachment);
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
    // MRT cleanup
    if (m_MRT && !m_ColorAttachments.empty()) {
        for (auto tex : m_ColorAttachments)
            VE_GPU_UNTRACK(GPUResourceType::Texture, tex);
        glDeleteTextures(static_cast<GLsizei>(m_ColorAttachments.size()), m_ColorAttachments.data());
        m_ColorAttachments.clear();
    }
    if (m_FBO) {
        VE_GPU_UNTRACK(GPUResourceType::Framebuffer, m_FBO);
        if (!m_MRT)
            VE_GPU_UNTRACK(GPUResourceType::Texture, m_ColorAttachment);
        VE_GPU_UNTRACK(GPUResourceType::Texture, m_DepthAttachment);
        glDeleteFramebuffers(1, &m_FBO);
        if (!m_MRT)
            glDeleteTextures(1, &m_ColorAttachment);
        glDeleteTextures(1, &m_DepthAttachment);
        m_FBO = 0;
        m_ColorAttachment = 0;
        m_DepthAttachment = 0;
    }
    if (m_ResolveFBO) {
        VE_GPU_UNTRACK(GPUResourceType::Framebuffer, m_ResolveFBO);
        VE_GPU_UNTRACK(GPUResourceType::Texture, m_ResolveColorAttachment);
        VE_GPU_UNTRACK(GPUResourceType::Texture, m_ResolveDepthAttachment);
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
