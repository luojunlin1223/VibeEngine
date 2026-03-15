#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace VE {

/// Color attachment format specification for MRT framebuffers.
/// Each entry describes one color attachment's internal format.
/// Common values: GL_RGBA8 (0x8058), GL_RGBA16F (0x881A), GL_RGBA32F (0x8814).
/// When empty, the framebuffer creates a single attachment using the HDR flag.
struct ColorAttachmentFormat {
    uint32_t InternalFormat; // e.g. GL_RGBA16F, GL_RGBA8
};

struct FramebufferSpec {
    uint32_t Width = 1280;
    uint32_t Height = 720;
    bool HDR = false;       // true → RGBA16F, false → RGBA8 (used when ColorFormats is empty)
    uint32_t Samples = 1;   // 1 = no MSAA, 2/4/8/16 = MSAA sample count

    /// MRT color attachment formats. When non-empty, overrides the HDR flag.
    /// Each entry creates one GL_COLOR_ATTACHMENTi with the specified internal format.
    std::vector<ColorAttachmentFormat> ColorFormats;
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

    /// Get color attachment texture ID by index (for MRT framebuffers).
    /// Index 0 is equivalent to GetColorAttachmentID().
    virtual uint64_t GetColorAttachmentID(int index) const { return GetColorAttachmentID(); }

    /// Number of color attachments (1 for standard, N for MRT).
    virtual int GetColorAttachmentCount() const { return 1; }

    // MSAA: resolve multisample FBO into a regular texture (returns resolved texture ID)
    virtual uint64_t Resolve() { return GetColorAttachmentID(); }
    virtual bool IsMultisampled() const { return false; }

    // Depth texture access (for SSAO etc.)
    virtual uint64_t GetDepthAttachmentID() const { return 0; }

    static std::shared_ptr<Framebuffer> Create(const FramebufferSpec& spec);
};

} // namespace VE
