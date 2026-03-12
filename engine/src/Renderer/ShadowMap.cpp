/*
 * ShadowMap — Cascaded Shadow Map implementation (OpenGL).
 *
 * Creates a GL_TEXTURE_2D_ARRAY with NUM_CASCADES layers of depth textures.
 * Each frame, ComputeCascades() splits the camera frustum and computes
 * tight orthographic projections from the light's point of view.
 */
#include "VibeEngine/Renderer/ShadowMap.h"
#include "VibeEngine/Renderer/Shader.h"
#include "VibeEngine/Core/Log.h"

#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace VE {

// ── Depth-only shaders (inline) ──────────────────────────────────────────

static const char* s_DepthVertexSrc = R"(
#version 460 core
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec3 a_Color;
layout(location = 3) in vec2 a_TexCoord;

uniform mat4 u_LightSpaceMatrix;
uniform mat4 u_Model;

void main() {
    gl_Position = u_LightSpaceMatrix * u_Model * vec4(a_Position, 1.0);
}
)";

static const char* s_DepthFragmentSrc = R"(
#version 460 core
void main() {
    // depth is written automatically
}
)";

// ── Constructor / Destructor ─────────────────────────────────────────────

ShadowMap::ShadowMap() {
    Init();
}

ShadowMap::~ShadowMap() {
    if (m_FBO) glDeleteFramebuffers(1, &m_FBO);
    if (m_DepthTexture) glDeleteTextures(1, &m_DepthTexture);
}

void ShadowMap::Init() {
    // Create depth shader
    m_DepthShader = Shader::Create(s_DepthVertexSrc, s_DepthFragmentSrc);

    // Create depth texture array
    glGenTextures(1, &m_DepthTexture);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_DepthTexture);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT32F,
                 MAP_SIZE, MAP_SIZE, NUM_CASCADES,
                 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    float borderColor[] = { 1.0f, 1.0f, 1.0f, 1.0f };
    glTexParameterfv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BORDER_COLOR, borderColor);
    // Hardware shadow comparison
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);

    // Create FBO (we'll attach individual layers per cascade during BeginPass)
    glGenFramebuffers(1, &m_FBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
    // Attach layer 0 initially just to validate
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, m_DepthTexture, 0, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        VE_ENGINE_ERROR("ShadowMap FBO incomplete!");

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    VE_ENGINE_INFO("ShadowMap created: {}x{} x {} cascades", MAP_SIZE, MAP_SIZE, NUM_CASCADES);
}

// ── Frustum corner extraction ────────────────────────────────────────────

std::vector<glm::vec4> ShadowMap::GetFrustumCornersWorldSpace(const glm::mat4& viewProj) const {
    const glm::mat4 inv = glm::inverse(viewProj);
    std::vector<glm::vec4> corners;
    corners.reserve(8);

    for (int x = 0; x < 2; ++x) {
        for (int y = 0; y < 2; ++y) {
            for (int z = 0; z < 2; ++z) {
                glm::vec4 pt = inv * glm::vec4(
                    2.0f * x - 1.0f,
                    2.0f * y - 1.0f,
                    2.0f * z - 1.0f,
                    1.0f);
                corners.push_back(pt / pt.w);
            }
        }
    }
    return corners;
}

// ── Cascade computation ──────────────────────────────────────────────────

void ShadowMap::ComputeCascades(const glm::mat4& viewMatrix,
                                 const glm::mat4& projMatrix,
                                 const glm::vec3& lightDir,
                                 float nearClip, float farClip) {
    // Practical split scheme (mix of logarithmic and uniform)
    const float lambda = 0.75f;
    const float ratio = farClip / nearClip;

    float cascadeEnds[NUM_CASCADES + 1];
    cascadeEnds[0] = nearClip;
    for (int i = 1; i <= NUM_CASCADES; ++i) {
        float p = static_cast<float>(i) / static_cast<float>(NUM_CASCADES);
        float log_split = nearClip * std::pow(ratio, p);
        float uniform_split = nearClip + (farClip - nearClip) * p;
        cascadeEnds[i] = lambda * log_split + (1.0f - lambda) * uniform_split;
    }

    // Store cascade far planes in view space (positive values)
    for (int i = 0; i < NUM_CASCADES; ++i)
        m_CascadeSplits[i] = cascadeEnds[i + 1];

    // For each cascade, compute the light-space matrix
    for (int i = 0; i < NUM_CASCADES; ++i) {
        float cascadeNear = cascadeEnds[i];
        float cascadeFar  = cascadeEnds[i + 1];

        // Build a projection matrix for this sub-frustum
        // We need to extract FOV and aspect from projMatrix to rebuild
        // Or we can just modify the near/far of the original projection
        // Simpler: rebuild perspective with same params but different near/far
        float fov = 2.0f * std::atan(1.0f / projMatrix[1][1]);
        float aspect = projMatrix[1][1] / projMatrix[0][0];

        glm::mat4 cascadeProj = glm::perspective(fov, aspect, cascadeNear, cascadeFar);
        glm::mat4 cascadeVP = cascadeProj * viewMatrix;

        auto corners = GetFrustumCornersWorldSpace(cascadeVP);

        // Compute frustum center
        glm::vec3 center(0.0f);
        for (const auto& c : corners)
            center += glm::vec3(c);
        center /= static_cast<float>(corners.size());

        // Light view matrix looking at the center from light direction
        glm::vec3 lightDirN = glm::normalize(lightDir);
        glm::mat4 lightView = glm::lookAt(center + lightDirN * 50.0f, center, glm::vec3(0, 1, 0));

        // Handle edge case: light direction nearly parallel to up vector
        if (std::abs(glm::dot(lightDirN, glm::vec3(0, 1, 0))) > 0.99f)
            lightView = glm::lookAt(center + lightDirN * 50.0f, center, glm::vec3(0, 0, 1));

        // Find bounding box of frustum corners in light space
        float minX =  std::numeric_limits<float>::max();
        float maxX = -std::numeric_limits<float>::max();
        float minY =  std::numeric_limits<float>::max();
        float maxY = -std::numeric_limits<float>::max();
        float minZ =  std::numeric_limits<float>::max();
        float maxZ = -std::numeric_limits<float>::max();

        for (const auto& c : corners) {
            glm::vec4 lc = lightView * c;
            minX = std::min(minX, lc.x);
            maxX = std::max(maxX, lc.x);
            minY = std::min(minY, lc.y);
            maxY = std::max(maxY, lc.y);
            minZ = std::min(minZ, lc.z);
            maxZ = std::max(maxZ, lc.z);
        }

        // Extend Z range to include shadow casters behind the frustum
        float zExtent = maxZ - minZ;
        minZ -= zExtent * 2.0f;
        maxZ += zExtent * 0.5f;

        // Orthographic projection that tightly fits the cascade
        glm::mat4 lightProj = glm::ortho(minX, maxX, minY, maxY, minZ, maxZ);

        // Stabilize shadow map: snap to texel grid to reduce shimmer
        glm::mat4 shadowMatrix = lightProj * lightView;
        glm::vec4 shadowOrigin = shadowMatrix * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        shadowOrigin *= static_cast<float>(MAP_SIZE) / 2.0f;
        glm::vec4 rounded = glm::round(shadowOrigin);
        glm::vec4 roundOffset = (rounded - shadowOrigin) * 2.0f / static_cast<float>(MAP_SIZE);
        lightProj[3][0] += roundOffset.x;
        lightProj[3][1] += roundOffset.y;

        m_LightSpaceMatrices[i] = lightProj * lightView;
    }
}

// ── Shadow pass ──────────────────────────────────────────────────────────

void ShadowMap::BeginPass(int cascadeIndex) {
    // Save current viewport
    glGetIntegerv(GL_VIEWPORT, m_SavedViewport);

    glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              m_DepthTexture, 0, cascadeIndex);
    glViewport(0, 0, MAP_SIZE, MAP_SIZE);
    glClear(GL_DEPTH_BUFFER_BIT);

    // Avoid peter-panning artifacts with front-face culling during shadow pass
    glEnable(GL_DEPTH_TEST);
    glCullFace(GL_FRONT);
}

void ShadowMap::EndPass() {
    // Restore back-face culling
    glCullFace(GL_BACK);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(m_SavedViewport[0], m_SavedViewport[1],
               m_SavedViewport[2], m_SavedViewport[3]);
}

// ── Bind for reading ─────────────────────────────────────────────────────

void ShadowMap::BindForReading(uint32_t textureUnit) const {
    glActiveTexture(GL_TEXTURE0 + textureUnit);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_DepthTexture);
}

} // namespace VE
