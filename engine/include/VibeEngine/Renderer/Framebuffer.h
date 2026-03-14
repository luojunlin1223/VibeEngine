#pragma once

#include <cstdint>
#include <memory>

namespace VE {

struct FramebufferSpec {
    uint32_t Width = 1280;
    uint32_t Height = 720;
    bool HDR = false;       // true → RGBA16F, false → RGBA8
    uint32_t Samples = 1;   // 1 = no MSAA, 2/4/8/16 = MSAA sample count
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

    // MSAA: resolve multisample FBO into a regular texture (returns resolved texture ID)
    virtual uint64_t Resolve() { return GetColorAttachmentID(); }
    virtual bool IsMultisampled() const { return false; }

    static std::shared_ptr<Framebuffer> Create(const FramebufferSpec& spec);
};

} // namespace VE
