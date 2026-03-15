/*
 * ReflectionProbe — Captures a cubemap from a world position for environment reflections.
 *
 * Renders the scene 6 times (once per cube face) into a GL_TEXTURE_CUBE_MAP.
 * The captured cubemap is then sampled in the PBR shader to provide image-based
 * lighting (IBL) reflections based on the reflection vector and roughness.
 *
 * Usage:
 *   1. Create a ReflectionProbe with desired resolution.
 *   2. Call Capture(scene, position, ...) to bake the cubemap.
 *   3. Bind the cubemap in the render pass with BindCubemap(textureUnit).
 *   4. In the PBR shader, sample u_ReflectionProbe with the reflection vector.
 */
#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <memory>

namespace VE {

class Scene;

class ReflectionProbe {
public:
    // Supported resolutions
    static constexpr int RES_64  = 64;
    static constexpr int RES_128 = 128;
    static constexpr int RES_256 = 256;
    static constexpr int RES_512 = 512;

    explicit ReflectionProbe(int resolution = RES_128);
    ~ReflectionProbe();

    // Non-copyable
    ReflectionProbe(const ReflectionProbe&) = delete;
    ReflectionProbe& operator=(const ReflectionProbe&) = delete;

    // Move
    ReflectionProbe(ReflectionProbe&& other) noexcept;
    ReflectionProbe& operator=(ReflectionProbe&& other) noexcept;

    /// Capture the scene from the given world position into the cubemap.
    /// viewProjection and cameraPos are the *editor* camera's data (not used for capture).
    /// The capture uses its own 90-degree FOV cameras for each face.
    void Capture(Scene& scene, const glm::vec3& position);

    /// Bind the cubemap texture to a given texture unit for sampling.
    void BindCubemap(uint32_t textureUnit) const;

    /// Unbind the cubemap.
    void UnbindCubemap(uint32_t textureUnit) const;

    uint32_t GetCubemapID() const { return m_CubemapTexture; }
    int GetResolution() const { return m_Resolution; }

    /// Returns true if the cubemap has been baked at least once.
    bool IsBaked() const { return m_Baked; }

private:
    void Init();
    void Cleanup();

    /// Render one face of the cubemap.
    void RenderFace(Scene& scene, int faceIndex, const glm::vec3& position);

    int m_Resolution = RES_128;

    uint32_t m_FBO = 0;
    uint32_t m_DepthRBO = 0;         // depth renderbuffer for capture
    uint32_t m_CubemapTexture = 0;   // GL_TEXTURE_CUBE_MAP

    bool m_Baked = false;

    // Saved viewport state during capture
    int m_SavedViewport[4] = {};
    int m_SavedFBO = 0;

    // View matrices for 6 cube faces (computed once)
    static const glm::mat4 s_FaceViewMatrices[6];
};

} // namespace VE
