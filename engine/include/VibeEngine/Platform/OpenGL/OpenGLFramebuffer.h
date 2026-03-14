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

    uint64_t GetColorAttachmentID() const override {
        return static_cast<uint64_t>(m_Multisampled ? m_ResolveColorAttachment : m_ColorAttachment);
    }
    uint32_t GetWidth() const override { return m_Width; }
    uint32_t GetHeight() const override { return m_Height; }

    uint64_t Resolve() override;
    bool IsMultisampled() const override { return m_Multisampled; }
    uint64_t GetDepthAttachmentID() const override {
        return static_cast<uint64_t>(m_Multisampled ? m_ResolveDepthAttachment : m_DepthAttachment);
    }

private:
    void Invalidate();
    void Cleanup();

    uint32_t m_FBO = 0;
    uint32_t m_ColorAttachment = 0;
    uint32_t m_DepthAttachment = 0;
    uint32_t m_Width, m_Height;
    bool m_HDR = false;

    // MSAA support
    uint32_t m_Samples = 1;
    bool m_Multisampled = false;
    uint32_t m_ResolveFBO = 0;
    uint32_t m_ResolveColorAttachment = 0;
    uint32_t m_ResolveDepthAttachment = 0;
};

} // namespace VE
