#pragma once

#include "VibeEngine/Renderer/Texture.h"
#include <string>
#include <memory>
#include <unordered_map>

namespace VE {

class ThumbnailCache {
public:
    // Returns a native texture ID for the given absolute path, or 0 if unavailable.
    // Use with ImGui::Image((ImTextureID)id, ...). Lazily loads on first request.
    uint64_t GetThumbnail(const std::string& absolutePath);

    void Clear();

private:
    std::unordered_map<std::string, std::shared_ptr<Texture2D>> m_Cache;
};

} // namespace VE
