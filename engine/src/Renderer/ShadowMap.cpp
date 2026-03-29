/*
 * ShadowMap — Cascaded Shadow Map implementation (OpenGL).
 *
 * Creates a GL_TEXTURE_2D_ARRAY with NUM_CASCADES layers of depth textures.
 * Each frame, ComputeCascades() splits the camera frustum and computes
 * tight orthographic projections from the light's point of view.
 */
#include "VibeEngine/Renderer/ShadowMap.h"
#include "VibeEngine/Renderer/Shader.h"
#include "VibeEngine/Renderer/GPUResourceTracker.h"
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
    if (m_FBO) { VE_GPU_UNTRACK(GPUResourceType::Framebuffer, m_FBO); glDeleteFramebuffers(1, &m_FBO); }
    if (m_DepthTexture) { VE_GPU_UNTRACK(GPUResourceType::Texture, m_DepthTexture); glDeleteTextures(1, &m_DepthTexture); }
}

void ShadowMap::Init() {
    // Create depth shader
    m_DepthShader = Shader::Create(s_DepthVertexSrc, s_DepthFragmentSrc);

    // Create depth texture array
    glGenTextures(1, &m_DepthTexture);
    VE_GPU_TRACK(GPUResourceType::Texture, m_DepthTexture);
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
    VE_GPU_TRACK(GPUResourceType::Framebuffer, m_FBO);
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

        // Compute frustum center and bounding sphere radius
        glm::vec3 center(0.0f);
        for (const auto& c : corners)
            center += glm::vec3(c);
        center /= static_cast<float>(corners.size());

        float radius = 0.0f;
        for (const auto& c : corners)
            radius = std::max(radius, glm::length(glm::vec3(c) - center));

        // Light view: place light far enough away (beyond bounding sphere)
        glm::vec3 lightDirN = glm::normalize(lightDir);
        float lightDist = radius * 3.0f;
        glm::vec3 lightEye = center + lightDirN * lightDist;
        glm::vec3 up(0, 1, 0);
        if (std::abs(glm::dot(lightDirN, up)) > 0.99f)
            up = glm::vec3(0, 0, 1);
        glm::mat4 lightView = glm::lookAt(lightEye, center, up);

        // Use sphere radius for XY, fixed Z range centered on light distance
        float zNear = 0.0f;
        float zFar  = lightDist + radius * 3.0f; // cover objects behind center too

        glm::mat4 lightProj = glm::ortho(-radius, radius, -radius, radius, zNear, zFar);

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

// ═══════════════════════════════════════════════════════════════════════════
// SpotLightShadowMap — single 2D depth texture with perspective projection
// ═══════════════════════════════════════════════════════════════════════════

SpotLightShadowMap::SpotLightShadowMap() {
    Init();
}

SpotLightShadowMap::~SpotLightShadowMap() {
    if (m_FBO) { VE_GPU_UNTRACK(GPUResourceType::Framebuffer, m_FBO); glDeleteFramebuffers(1, &m_FBO); }
    if (m_DepthTexture) { VE_GPU_UNTRACK(GPUResourceType::Texture, m_DepthTexture); glDeleteTextures(1, &m_DepthTexture); }
}

void SpotLightShadowMap::Init() {
    m_DepthShader = Shader::Create(s_DepthVertexSrc, s_DepthFragmentSrc);

    glGenTextures(1, &m_DepthTexture);
    VE_GPU_TRACK(GPUResourceType::Texture, m_DepthTexture);
    glBindTexture(GL_TEXTURE_2D, m_DepthTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F,
                 MAP_SIZE, MAP_SIZE, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    float borderColor[] = { 1.0f, 1.0f, 1.0f, 1.0f };
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);

    glGenFramebuffers(1, &m_FBO);
    VE_GPU_TRACK(GPUResourceType::Framebuffer, m_FBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_DepthTexture, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        VE_ENGINE_ERROR("SpotLightShadowMap FBO incomplete!");

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    VE_ENGINE_INFO("SpotLightShadowMap created: {}x{}", MAP_SIZE, MAP_SIZE);
}

void SpotLightShadowMap::ComputeMatrix(const glm::vec3& lightPos, const glm::vec3& lightDir,
                                        float outerAngle, float range) {
    glm::vec3 dir = glm::normalize(lightDir);

    // Choose an up vector that isn't parallel to the light direction
    glm::vec3 up(0.0f, 1.0f, 0.0f);
    if (std::abs(glm::dot(dir, up)) > 0.99f)
        up = glm::vec3(0.0f, 0.0f, 1.0f);

    glm::mat4 lightView = glm::lookAt(lightPos, lightPos + dir, up);

    // Perspective projection matching the spot light cone
    float fov = glm::radians(outerAngle) * 2.0f; // full cone angle
    fov = std::min(fov, glm::radians(170.0f));     // clamp to avoid degenerate projections
    glm::mat4 lightProj = glm::perspective(fov, 1.0f, 0.1f, range);

    m_LightSpaceMatrix = lightProj * lightView;
}

void SpotLightShadowMap::BeginPass() {
    glGetIntegerv(GL_VIEWPORT, m_SavedViewport);
    glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
    glViewport(0, 0, MAP_SIZE, MAP_SIZE);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glCullFace(GL_FRONT);
}

void SpotLightShadowMap::EndPass() {
    glCullFace(GL_BACK);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(m_SavedViewport[0], m_SavedViewport[1],
               m_SavedViewport[2], m_SavedViewport[3]);
}

void SpotLightShadowMap::BindForReading(uint32_t textureUnit) const {
    glActiveTexture(GL_TEXTURE0 + textureUnit);
    glBindTexture(GL_TEXTURE_2D, m_DepthTexture);
}

// ═══════════════════════════════════════════════════════════════════════════
// PointLightShadowMap — cube map depth texture for omnidirectional shadows
// ═══════════════════════════════════════════════════════════════════════════

// Depth shader for point light shadows — writes linear depth for proper
// distance-based comparison in the fragment shader.

static const char* s_PointDepthVertexSrc = R"(
#version 460 core
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec3 a_Color;
layout(location = 3) in vec2 a_TexCoord;

uniform mat4 u_LightSpaceMatrix;
uniform mat4 u_Model;

out vec3 v_FragPos;

void main() {
    v_FragPos = vec3(u_Model * vec4(a_Position, 1.0));
    gl_Position = u_LightSpaceMatrix * vec4(v_FragPos, 1.0);
}
)";

static const char* s_PointDepthFragmentSrc = R"(
#version 460 core
in vec3 v_FragPos;

uniform vec3  u_LightPos;
uniform float u_FarPlane;

void main() {
    float dist = length(v_FragPos - u_LightPos);
    gl_FragDepth = dist / u_FarPlane; // normalize to [0, 1]
}
)";

PointLightShadowMap::PointLightShadowMap() {
    Init();
}

PointLightShadowMap::~PointLightShadowMap() {
    if (m_FBO) { VE_GPU_UNTRACK(GPUResourceType::Framebuffer, m_FBO); glDeleteFramebuffers(1, &m_FBO); }
    if (m_DepthCubeMap) { VE_GPU_UNTRACK(GPUResourceType::Texture, m_DepthCubeMap); glDeleteTextures(1, &m_DepthCubeMap); }
}

void PointLightShadowMap::Init() {
    m_DepthShader = Shader::Create(s_PointDepthVertexSrc, s_PointDepthFragmentSrc);

    // Create cube map depth texture
    glGenTextures(1, &m_DepthCubeMap);
    VE_GPU_TRACK(GPUResourceType::Texture, m_DepthCubeMap);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_DepthCubeMap);
    for (int i = 0; i < 6; ++i) {
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_DEPTH_COMPONENT32F,
                     MAP_SIZE, MAP_SIZE, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

    // Create FBO
    glGenFramebuffers(1, &m_FBO);
    VE_GPU_TRACK(GPUResourceType::Framebuffer, m_FBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                           GL_TEXTURE_CUBE_MAP_POSITIVE_X, m_DepthCubeMap, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        VE_ENGINE_ERROR("PointLightShadowMap FBO incomplete!");

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    VE_ENGINE_INFO("PointLightShadowMap created: {}x{} cube map", MAP_SIZE, MAP_SIZE);
}

void PointLightShadowMap::ComputeMatrices(const glm::vec3& lightPos, float farPlane) {
    m_FarPlane = farPlane;

    glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, farPlane);

    // Standard cube map face view matrices
    m_LightSpaceMatrices[0] = proj * glm::lookAt(lightPos, lightPos + glm::vec3( 1, 0, 0), glm::vec3(0,-1, 0)); // +X
    m_LightSpaceMatrices[1] = proj * glm::lookAt(lightPos, lightPos + glm::vec3(-1, 0, 0), glm::vec3(0,-1, 0)); // -X
    m_LightSpaceMatrices[2] = proj * glm::lookAt(lightPos, lightPos + glm::vec3( 0, 1, 0), glm::vec3(0, 0, 1)); // +Y
    m_LightSpaceMatrices[3] = proj * glm::lookAt(lightPos, lightPos + glm::vec3( 0,-1, 0), glm::vec3(0, 0,-1)); // -Y
    m_LightSpaceMatrices[4] = proj * glm::lookAt(lightPos, lightPos + glm::vec3( 0, 0, 1), glm::vec3(0,-1, 0)); // +Z
    m_LightSpaceMatrices[5] = proj * glm::lookAt(lightPos, lightPos + glm::vec3( 0, 0,-1), glm::vec3(0,-1, 0)); // -Z
}

void PointLightShadowMap::BeginPass(int face) {
    glGetIntegerv(GL_VIEWPORT, m_SavedViewport);
    glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                           GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, m_DepthCubeMap, 0);
    glViewport(0, 0, MAP_SIZE, MAP_SIZE);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glCullFace(GL_FRONT);
}

void PointLightShadowMap::EndPass() {
    glCullFace(GL_BACK);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(m_SavedViewport[0], m_SavedViewport[1],
               m_SavedViewport[2], m_SavedViewport[3]);
}

void PointLightShadowMap::BindForReading(uint32_t textureUnit) const {
    glActiveTexture(GL_TEXTURE0 + textureUnit);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_DepthCubeMap);
}

} // namespace VE
