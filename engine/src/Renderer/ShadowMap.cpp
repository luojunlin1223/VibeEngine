/*
 * ShadowMap — Cascaded Shadow Maps (CSM) implementation.
 *
 * Uses a single GL_TEXTURE_2D_ARRAY with 4 layers (one per cascade).
 * Each frame: compute cascade frustum splits, build light-space matrices,
 * then render shadow casters to each layer via glFramebufferTextureLayer.
 */

#include "VibeEngine/Renderer/ShadowMap.h"
#include "VibeEngine/Renderer/Shader.h"
#include "VibeEngine/Core/Log.h"
#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace VE {

// ── Depth-only shader sources ────────────────────────────────────────

static const char* s_DepthVertSrc = R"(
#version 460 core
layout(location = 0) in vec3 a_Position;
uniform mat4 u_LightMVP;
void main() {
    gl_Position = u_LightMVP * vec4(a_Position, 1.0);
}
)";

static const char* s_DepthFragSrc = R"(
#version 460 core
void main() {
    // depth written automatically by the rasterizer
}
)";

// ── Lifecycle ────────────────────────────────────────────────────────

ShadowMap::~ShadowMap() {
    Shutdown();
}

void ShadowMap::Init(int resolution) {
    if (m_Initialized) return;
    m_Resolution = resolution;
    CreateGLResources();
    m_Initialized = true;
    VE_ENGINE_INFO("ShadowMap initialized: {}x{} x {} cascades", m_Resolution, m_Resolution, NUM_CASCADES);
}

void ShadowMap::Shutdown() {
    if (!m_Initialized) return;
    DestroyGLResources();
    m_DepthShader.reset();
    m_Initialized = false;
}

void ShadowMap::Resize(int resolution) {
    if (resolution == m_Resolution && m_Initialized) return;
    if (m_Initialized) DestroyGLResources();
    m_Resolution = resolution;
    CreateGLResources();
    m_Initialized = true;
    VE_ENGINE_INFO("ShadowMap resized: {}x{}", m_Resolution, m_Resolution);
}

// ── GL Resource Management ───────────────────────────────────────────

void ShadowMap::CreateGLResources() {
    // Create depth texture array (4 layers)
    glGenTextures(1, &m_DepthTexArray);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_DepthTexArray);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT32F,
                 m_Resolution, m_Resolution, NUM_CASCADES,
                 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);

    // No hardware comparison — manual depth compare in shader (sampler2DArray)
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_NONE);

    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // Clamp to border with depth=1.0 (no shadow outside map)
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    float borderColor[] = { 1.0f, 1.0f, 1.0f, 1.0f };
    glTexParameterfv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BORDER_COLOR, borderColor);

    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

    // Create FBO (layer attachment switched per cascade pass)
    glGenFramebuffers(1, &m_FBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
    // Attach first layer initially to validate FBO completeness
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, m_DepthTexArray, 0, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        VE_ENGINE_ERROR("ShadowMap FBO incomplete: 0x{:X}", status);
    }

    // Explicitly clear all cascade layers to depth=1.0
    glDepthMask(GL_TRUE);
    const float clearDepth = 1.0f;
    for (int i = 0; i < NUM_CASCADES; ++i) {
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, m_DepthTexArray, 0, i);
        glClearBufferfv(GL_DEPTH, 0, &clearDepth);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void ShadowMap::DestroyGLResources() {
    if (m_DepthTexArray) { glDeleteTextures(1, &m_DepthTexArray); m_DepthTexArray = 0; }
    if (m_FBO) { glDeleteFramebuffers(1, &m_FBO); m_FBO = 0; }
}

// ── Depth Shader ─────────────────────────────────────────────────────

std::shared_ptr<Shader> ShadowMap::GetDepthShader() {
    if (!m_DepthShader) {
        m_DepthShader = Shader::Create(s_DepthVertSrc, s_DepthFragSrc);
        if (m_DepthShader) {
            m_DepthShader->SetName("ShadowDepth");
            VE_ENGINE_INFO("ShadowMap: depth shader created successfully");
        } else {
            VE_ENGINE_ERROR("ShadowMap: FAILED to create depth shader!");
        }
    }
    return m_DepthShader;
}

// ── Cascade Split Computation ────────────────────────────────────────
// Practical split scheme (GPU Gems 3, Chapter 10):
//   split[i] = lambda * log_split + (1 - lambda) * uniform_split

void ShadowMap::ComputeCascadeSplits(float nearClip, float farClip, float lambda) {
    for (int i = 0; i < NUM_CASCADES; ++i) {
        float p = static_cast<float>(i + 1) / static_cast<float>(NUM_CASCADES);
        float logSplit     = nearClip * std::pow(farClip / nearClip, p);
        float uniformSplit = nearClip + (farClip - nearClip) * p;
        m_CascadeSplits[i] = lambda * logSplit + (1.0f - lambda) * uniformSplit;
    }
}

// ── Light-Space Matrix Computation ───────────────────────────────────

glm::mat4 ShadowMap::ComputeLightMatrix(const glm::mat4& camView, const glm::mat4& camProj,
                                          const glm::vec3& lightDir,
                                          float splitNear, float splitFar) {
    // 1. Extract the 8 corners of the full camera frustum from NDC to world space.
    glm::mat4 invVP = glm::inverse(camProj * camView);

    glm::vec3 fullCorners[8] = {
        { -1.0f, -1.0f, -1.0f }, {  1.0f, -1.0f, -1.0f },
        {  1.0f,  1.0f, -1.0f }, { -1.0f,  1.0f, -1.0f },
        { -1.0f, -1.0f,  1.0f }, {  1.0f, -1.0f,  1.0f },
        {  1.0f,  1.0f,  1.0f }, { -1.0f,  1.0f,  1.0f },
    };

    for (auto& corner : fullCorners) {
        glm::vec4 w = invVP * glm::vec4(corner, 1.0f);
        corner = glm::vec3(w) / w.w;
    }

    // 2. Interpolate along near -> far rays to get this cascade sub-frustum.
    float projNear = camProj[3][2] / (camProj[2][2] - 1.0f);
    float projFar  = camProj[3][2] / (camProj[2][2] + 1.0f);
    float range = std::max(projFar - projNear, 0.001f);

    float tNear = glm::clamp((splitNear - projNear) / range, 0.0f, 1.0f);
    float tFar  = glm::clamp((splitFar  - projNear) / range, 0.0f, 1.0f);

    glm::vec3 subCorners[8];
    for (int i = 0; i < 4; ++i) {
        subCorners[i]     = glm::mix(fullCorners[i], fullCorners[i + 4], tNear);
        subCorners[i + 4] = glm::mix(fullCorners[i], fullCorners[i + 4], tFar);
    }

    // 3. Use a bounding sphere for stable cascades. The light camera sits on
    // the incoming-light side and looks at the cascade center.
    glm::vec3 center(0.0f);
    for (const auto& c : subCorners)
        center += c;
    center /= 8.0f;

    float radius = 0.0f;
    for (const auto& c : subCorners)
        radius = std::max(radius, glm::length(c - center));
    radius = std::ceil(radius * 16.0f) / 16.0f;
    radius = std::max(radius, 1.0f);

    glm::vec3 L = glm::normalize(lightDir); // direction from surface toward the light
    glm::vec3 up(0.0f, 1.0f, 0.0f);
    if (std::abs(glm::dot(L, up)) > 0.95f)
        up = glm::vec3(0.0f, 0.0f, 1.0f);

    const float casterMargin = radius * 1.5f;
    glm::vec3 eye = center + L * (radius + casterMargin);
    glm::mat4 lightView = glm::lookAt(eye, center, up);

    // 4. Transform sub-frustum corners to light view space and compute an AABB.
    float minX =  1e30f;
    float maxX = -1e30f;
    float minY =  1e30f;
    float maxY = -1e30f;
    float minZ =  1e30f;
    float maxZ = -1e30f;

    for (const auto& c : subCorners) {
        glm::vec3 lv = glm::vec3(lightView * glm::vec4(c, 1.0f));
        minX = std::min(minX, lv.x); maxX = std::max(maxX, lv.x);
        minY = std::min(minY, lv.y); maxY = std::max(maxY, lv.y);
        minZ = std::min(minZ, lv.z); maxZ = std::max(maxZ, lv.z);
    }

    // Stabilize XY to a square footprint; this avoids shimmering and keeps texel
    // density stable as the camera rotates.
    glm::vec3 centerLS = glm::vec3(lightView * glm::vec4(center, 1.0f));

    float halfExtent = std::max(maxX - minX, maxY - minY) * 0.5f;
    halfExtent = std::max(halfExtent, radius);
    minX = centerLS.x - halfExtent;
    maxX = centerLS.x + halfExtent;
    minY = centerLS.y - halfExtent;
    maxY = centerLS.y + halfExtent;

    // Texel snapping in light-space. Move the projected center to shadow texel
    // increments so shadow edges don't swim when the camera moves.
    float worldUnitsPerTexel = (halfExtent * 2.0f) / static_cast<float>(m_Resolution);
    if (worldUnitsPerTexel > 0.0f) {
        float snappedX = std::floor(centerLS.x / worldUnitsPerTexel) * worldUnitsPerTexel;
        float snappedY = std::floor(centerLS.y / worldUnitsPerTexel) * worldUnitsPerTexel;
        float dx = snappedX - centerLS.x;
        float dy = snappedY - centerLS.y;
        minX += dx; maxX += dx;
        minY += dy; maxY += dy;
    }

    // OpenGL light view uses negative Z forward. Convert the light-space z AABB
    // into positive near/far distances and add a caster margin behind the cascade.
    float nearPlane = std::max(0.01f, -maxZ - casterMargin);
    float farPlane  = std::max(nearPlane + 1.0f, -minZ + casterMargin);
    glm::mat4 lightOrtho = glm::ortho(minX, maxX, minY, maxY, nearPlane, farPlane);

    return lightOrtho * lightView;
}

// ── Per-Frame Update ─────────────────────────────────────────────────

void ShadowMap::Update(const glm::mat4& view, const glm::mat4& projection,
                        const glm::vec3& lightDir, float nearClip, float farClip,
                        const ShadowSettings& settings) {
    if (!m_Initialized) return;

    // Clamp far clip to max shadow distance
    float shadowFar = std::min(farClip, settings.MaxDistance);

    ComputeCascadeSplits(nearClip, shadowFar, settings.SplitLambda);

    // Compute light-space VP matrix for each cascade
    float prevSplit = nearClip;
    for (int i = 0; i < NUM_CASCADES; ++i) {
        m_LightVP[i] = ComputeLightMatrix(view, projection, lightDir, prevSplit, m_CascadeSplits[i]);
        prevSplit = m_CascadeSplits[i];
    }
}

// ── Shadow Depth Rendering ───────────────────────────────────────────

void ShadowMap::BeginCascadePass(int cascadeIndex) {
    glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, m_DepthTexArray, 0, cascadeIndex);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    glViewport(0, 0, m_Resolution, m_Resolution);

    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);

    // CRITICAL: depth mask must be TRUE before glClear, otherwise clear is a no-op
    glDepthMask(GL_TRUE);
    const float clearDepth = 1.0f;
    glClearBufferfv(GL_DEPTH, 0, &clearDepth);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    // Disable color write (depth-only pass)
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT);

    // Render back faces into the shadow map. This keeps one-sided receiver
    // planes from self-shadowing if they accidentally enter the pass; the
    // shader applies a small receiver-depth push to avoid contact light leaks.
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.0f, 2.0f);
}

void ShadowMap::EndCascadePass() {
    glDisable(GL_POLYGON_OFFSET_FILL);
    glCullFace(GL_BACK);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// ── Shader Binding ───────────────────────────────────────────────────

static const char* s_LightVPNames[ShadowMap::NUM_CASCADES] = {
    "u_ShadowLightVP[0]", "u_ShadowLightVP[1]",
    "u_ShadowLightVP[2]", "u_ShadowLightVP[3]"
};

static const char* s_CascadeSplitNames[ShadowMap::NUM_CASCADES] = {
    "u_ShadowCascadeSplits[0]", "u_ShadowCascadeSplits[1]",
    "u_ShadowCascadeSplits[2]", "u_ShadowCascadeSplits[3]"
};

void ShadowMap::BindToShader(const std::shared_ptr<Shader>& shader, const ShadowSettings& settings) {
    if (!shader || !m_Initialized) return;

    // Bind texture array
    glActiveTexture(GL_TEXTURE0 + TEXTURE_UNIT);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_DepthTexArray);
    shader->SetInt("u_ShadowMap", TEXTURE_UNIT);

    shader->SetInt("u_ShadowsEnabled", settings.Enabled ? 1 : 0);
    shader->SetFloat("u_ShadowDepthBias", settings.DepthBias);
    shader->SetFloat("u_ShadowNormalBias", settings.NormalBias);
    shader->SetInt("u_ShadowPCFQuality", settings.PCFQuality);
    shader->SetFloat("u_ShadowCascadeBlendWidth", settings.CascadeBlendWidth);

    for (int i = 0; i < NUM_CASCADES; ++i) {
        shader->SetMat4(s_LightVPNames[i], m_LightVP[i]);
        shader->SetFloat(s_CascadeSplitNames[i], m_CascadeSplits[i]);
    }
}

} // namespace VE
