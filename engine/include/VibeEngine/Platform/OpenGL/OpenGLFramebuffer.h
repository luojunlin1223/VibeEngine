#pragma once

#include "VibeEngine/Renderer/Framebuffer.h"

namespace VE {

class OpenGLFramebuffer : public Framebuffer {
public:
    OpenGLFramebuffer(const FramebufferSpec& spec);
    ~OpenGLFramebuffer() override;

    void Bind() override;
    void Unbind() override;
    void Resize(uint32_t width, uint32_t height) override;

    uint64_t GetColorAttachmentID() const override { return static_cast<uint64_t>(m_ColorAttachment); }
    uint32_t GetWidth() const override { return m_Width; }
    uint32_t GetHeight() const override { return m_Height; }

private:
    void Invalidate();
    void Cleanup();

    uint32_t m_FBO = 0;
    uint32_t m_ColorAttachment = 0;
    uint32_t m_DepthAttachment = 0;
    uint32_t m_Width, m_Height;
    bool m_HDR = false;
};

} // namespace VE
