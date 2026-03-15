/*
 * Lightmapper -- CPU-side lightmap baker for static geometry.
 *
 * For each entity marked as static, the baker:
 *   1. Iterates lightmap texels in UV space.
 *   2. For each texel with valid UV coverage, computes the world-space
 *      position and normal from the mesh geometry.
 *   3. Evaluates direct lighting from all scene lights (directional,
 *      point) with shadow ray tests (simple dot-product visibility).
 *   4. Stores the result into a Texture2D that is bound at render time
 *      to replace direct lighting on static objects (much cheaper).
 *
 * This is a v1 direct-light-only baker (no bounced light).
 */
#pragma once

#include "VibeEngine/Renderer/Texture.h"
#include <glm/glm.hpp>
#include <memory>
#include <vector>
#include <string>
#include <functional>

namespace VE {

class Scene;

// ── Lightmap baker settings ───────────────────────────────────────────

struct LightmapSettings {
    int Resolution = 128;        // per-object lightmap resolution (64, 128, 256, 512)
    float AmbientIntensity = 0.03f; // base ambient added to each texel
    int SampleCount = 1;          // number of shadow samples per texel (1 = no soft shadow)
};

// ── Lightmapper ───────────────────────────────────────────────────────

class Lightmapper {
public:
    /// Progress callback: (currentStep, totalSteps) -- called during baking.
    using ProgressCallback = std::function<void(int current, int total)>;

    /// Bake lightmaps for all entities in the scene that have
    /// LightmapComponent with IsStatic=true and a MeshRendererComponent.
    /// The baked Texture2D is stored directly in each entity's LightmapComponent.
    static void BakeScene(Scene& scene, const LightmapSettings& settings,
                          ProgressCallback progress = nullptr);

    /// Bake a single lightmap for one entity.
    /// Returns the lightmap texture (RGBA8).
    static std::shared_ptr<Texture2D> BakeEntity(
        Scene& scene,
        const glm::mat4& modelMatrix,
        const std::vector<float>& vertices,   // interleaved: pos(3)+normal(3)+color(3)+uv(2) = 11 floats
        const std::vector<uint32_t>& indices,
        int resolution,
        float ambientIntensity);

    /// Save a lightmap to disk as raw RGBA data (for caching).
    static bool SaveLightmap(const std::string& path, const std::shared_ptr<Texture2D>& texture,
                             uint32_t width, uint32_t height);

    /// Load a lightmap from disk.
    static std::shared_ptr<Texture2D> LoadLightmap(const std::string& path);
};

} // namespace VE
