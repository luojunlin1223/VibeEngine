#pragma once

#include <cstdint>
#include <memory>

namespace VE {

struct FramebufferSpec {
    uint32_t Width = 1280;
    uint32_t Height = 720;
    bool HDR = false;   // true → RGBA16F, false → RGBA8
};

class Framebuffer {
public:
    virtual ~Framebuffer() = default;

    virtual void Bind() = 0;
    virtual void Unbind() = 0;
    virtual void Resize(uint32_t width, uint32_t height) = 0;

    virtual uint64_t GetColorAttachmentID() const = 0;
    virtual uint32_t GetWidth() const = 0;
    virtual uint32_t GetHeight() const = 0;

    static std::shared_ptr<Framebuffer> Create(const FramebufferSpec& spec);
};

} // namespace VE
