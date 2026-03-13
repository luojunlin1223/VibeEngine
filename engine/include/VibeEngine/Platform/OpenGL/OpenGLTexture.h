#pragma once

#include "VibeEngine/Renderer/Texture.h"
#include <string>

namespace VE {

class OpenGLTexture2D : public Texture2D {
public:
    explicit OpenGLTexture2D(const std::string& path);
    OpenGLTexture2D(uint32_t width, uint32_t height, const void* data);
    ~OpenGLTexture2D() override;

    uint32_t GetWidth() const override { return m_Width; }
    uint32_t GetHeight() const override { return m_Height; }
    void Bind(uint32_t slot = 0) const override;
    void Unbind() const override;
    uint64_t GetNativeTextureID() const override { return static_cast<uint64_t>(m_RendererID); }

private:
    uint32_t m_RendererID = 0;
    uint32_t m_Width  = 0;
    uint32_t m_Height = 0;
    std::string m_FilePath;
};

} // namespace VE
