#pragma once

#include <string>
#include <memory>
#include <cstdint>

namespace VE {

class Texture2D {
public:
    virtual ~Texture2D() = default;

    virtual uint32_t GetWidth() const = 0;
    virtual uint32_t GetHeight() const = 0;
    virtual void Bind(uint32_t slot = 0) const = 0;
    virtual void Unbind() const = 0;

    // Returns the native texture handle as a uint64 suitable for ImGui::Image()
    virtual uint64_t GetNativeTextureID() const = 0;

    static std::shared_ptr<Texture2D> Create(const std::string& path);
};

} // namespace VE
