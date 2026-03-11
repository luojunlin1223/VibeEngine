#include "VibeEngine/Asset/ThumbnailCache.h"
#include "VibeEngine/Core/Log.h"

namespace VE {

uint64_t ThumbnailCache::GetThumbnail(const std::string& absolutePath) {
    auto it = m_Cache.find(absolutePath);
    if (it != m_Cache.end()) {
        if (it->second) {
            uint64_t id = it->second->GetNativeTextureID();
            if (id != 0) return id;
        }
        return 0; // cached as failed
    }

    auto tex = Texture2D::Create(absolutePath);
    if (tex && tex->GetNativeTextureID() != 0) {
        m_Cache[absolutePath] = tex;
        return tex->GetNativeTextureID();
    }

    m_Cache[absolutePath] = nullptr; // mark as failed, don't retry every frame
    return 0;
}

void ThumbnailCache::Clear() {
    m_Cache.clear();
}

} // namespace VE
