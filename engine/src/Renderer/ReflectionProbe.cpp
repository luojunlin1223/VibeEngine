/*
 * ReflectionProbe — Cubemap capture implementation (OpenGL).
 *
 * Creates a GL_TEXTURE_CUBE_MAP and an FBO. For each capture, renders the scene
 * 6 times with 90-degree FOV perspective cameras oriented along each axis.
 * The resulting cubemap can be sampled in the PBR shader for reflections.
 */
#include "VibeEngine/Renderer/ReflectionProbe.h"
#include "VibeEngine/Scene/Scene.h"
#include "VibeEngine/Scene/Components.h"
#include "VibeEngine/Scene/MeshLibrary.h"
#include "VibeEngine/Renderer/RenderCommand.h"
#include "VibeEngine/Core/Log.h"

#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>

namespace VE {

// ── Static face view directions ──────────────────────────────────────────
// These are the standard cubemap face orientations (OpenGL convention):
//   +X: look right    -X: look left
//   +Y: look up       -Y: look down
//   +Z: look forward  -Z: look backward

const glm::mat4 ReflectionProbe::s_FaceViewMatrices[6] = {
    // +X
    glm::lookAt(glm::vec3(0.0f), glm::vec3( 1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)),
    // -X
    glm::lookAt(glm::vec3(0.0f), glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)),
    // +Y
    glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f,  1.0f, 0.0f), glm::vec3(0.0f, 0.0f,  1.0f)),
    // -Y
    glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f)),
    // +Z
    glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f,  1.0f), glm::vec3(0.0f, -1.0f, 0.0f)),
    // -Z
    glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, -1.0f, 0.0f)),
};

// ── Constructor / Destructor ─────────────────────────────────────────────

ReflectionProbe::ReflectionProbe(int resolution)
    : m_Resolution(resolution)
{
    Init();
}

ReflectionProbe::~ReflectionProbe() {
    Cleanup();
}

ReflectionProbe::ReflectionProbe(ReflectionProbe&& other) noexcept
    : m_Resolution(other.m_Resolution)
    , m_FBO(other.m_FBO)
    , m_DepthRBO(other.m_DepthRBO)
    , m_CubemapTexture(other.m_CubemapTexture)
    , m_Baked(other.m_Baked)
{
    other.m_FBO = 0;
    other.m_DepthRBO = 0;
    other.m_CubemapTexture = 0;
    other.m_Baked = false;
}

ReflectionProbe& ReflectionProbe::operator=(ReflectionProbe&& other) noexcept {
    if (this != &other) {
        Cleanup();
        m_Resolution = other.m_Resolution;
        m_FBO = other.m_FBO;
        m_DepthRBO = other.m_DepthRBO;
        m_CubemapTexture = other.m_CubemapTexture;
        m_Baked = other.m_Baked;
        other.m_FBO = 0;
        other.m_DepthRBO = 0;
        other.m_CubemapTexture = 0;
        other.m_Baked = false;
    }
    return *this;
}

// ── Init / Cleanup ───────────────────────────────────────────────────────

void ReflectionProbe::Init() {
    // Create cubemap texture
    glGenTextures(1, &m_CubemapTexture);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_CubemapTexture);

    for (int i = 0; i < 6; ++i) {
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_RGB16F,
                     m_Resolution, m_Resolution, 0, GL_RGB, GL_FLOAT, nullptr);
    }

    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

    // Create depth renderbuffer
    glGenRenderbuffers(1, &m_DepthRBO);
    glBindRenderbuffer(GL_RENDERBUFFER, m_DepthRBO);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, m_Resolution, m_Resolution);

    // Create FBO
    glGenFramebuffers(1, &m_FBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_DepthRBO);

    // Attach face 0 to validate
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_CUBE_MAP_POSITIVE_X, m_CubemapTexture, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        VE_ENGINE_ERROR("ReflectionProbe FBO incomplete!");
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);

    VE_ENGINE_INFO("ReflectionProbe created: {}x{} cubemap", m_Resolution, m_Resolution);
}

void ReflectionProbe::Cleanup() {
    if (m_FBO) { glDeleteFramebuffers(1, &m_FBO); m_FBO = 0; }
    if (m_DepthRBO) { glDeleteRenderbuffers(1, &m_DepthRBO); m_DepthRBO = 0; }
    if (m_CubemapTexture) { glDeleteTextures(1, &m_CubemapTexture); m_CubemapTexture = 0; }
}

// ── Capture ──────────────────────────────────────────────────────────────

void ReflectionProbe::Capture(Scene& scene, const glm::vec3& position) {
    // Save current GL state
    glGetIntegerv(GL_VIEWPORT, m_SavedViewport);
    GLint currentFBO = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &currentFBO);
    m_SavedFBO = currentFBO;

    glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
    glViewport(0, 0, m_Resolution, m_Resolution);

    // Render each face
    for (int face = 0; face < 6; ++face) {
        RenderFace(scene, face, position);
    }

    // Generate mipmaps for roughness-based sampling
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_CubemapTexture);
    glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);

    // Restore GL state
    glBindFramebuffer(GL_FRAMEBUFFER, m_SavedFBO);
    glViewport(m_SavedViewport[0], m_SavedViewport[1],
               m_SavedViewport[2], m_SavedViewport[3]);

    m_Baked = true;
    VE_ENGINE_INFO("ReflectionProbe baked at ({:.1f}, {:.1f}, {:.1f}), {}x{}",
                   position.x, position.y, position.z, m_Resolution, m_Resolution);
}

void ReflectionProbe::RenderFace(Scene& scene, int faceIndex, const glm::vec3& position) {
    // Attach the cubemap face to the FBO color attachment
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_CUBE_MAP_POSITIVE_X + faceIndex,
                           m_CubemapTexture, 0);

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // 90-degree FOV, 1:1 aspect ratio, standard near/far
    glm::mat4 projection = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 1000.0f);

    // Translate the face view matrix to the probe position
    glm::mat4 view = s_FaceViewMatrices[faceIndex] * glm::translate(glm::mat4(1.0f), -position);

    glm::mat4 viewProjection = projection * view;

    // Render sky first (strip translation from view for sky)
    glm::mat4 skyView = glm::mat4(glm::mat3(view)); // remove translation
    glm::mat4 skyVP = projection * skyView;
    scene.OnRenderSky(skyVP);

    // Render scene geometry
    scene.OnRender(viewProjection, position);

    // Render terrain if present
    scene.OnRenderTerrain(viewProjection, position);
}

// ── Bind / Unbind ────────────────────────────────────────────────────────

void ReflectionProbe::BindCubemap(uint32_t textureUnit) const {
    glActiveTexture(GL_TEXTURE0 + textureUnit);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_CubemapTexture);
}

void ReflectionProbe::UnbindCubemap(uint32_t textureUnit) const {
    glActiveTexture(GL_TEXTURE0 + textureUnit);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
}

} // namespace VE
