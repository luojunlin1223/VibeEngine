/*
 * LightProbe -- Bakes environment lighting into L2 Spherical Harmonics.
 *
 * The baking process:
 *   1. Create a small cubemap FBO (6 faces, e.g. 64x64 each).
 *   2. Render the scene from the probe position into each face.
 *   3. Read back the pixels and project them onto the SH basis.
 *   4. Store the resulting 9 * vec3 coefficients for runtime use.
 *
 * At runtime the shader evaluates SH to get per-fragment irradiance,
 * replacing the constant ambient term.
 */
#include "VibeEngine/Renderer/LightProbe.h"
#include "VibeEngine/Scene/Scene.h"
#include "VibeEngine/Core/Log.h"

#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <algorithm>

namespace VE {

// ── SH basis functions (L2, order 2, real) ────────────────────────────
// Uses the convention from "Stupid Spherical Harmonics Tricks" (P. Sloan)
// and "An Efficient Representation for Irradiance Environment Maps" (Ramamoorthi & Hanrahan).

static constexpr float SH_C0 = 0.282094791773878f;  // 1/(2*sqrt(PI))
static constexpr float SH_C1 = 0.488602511902920f;  // sqrt(3)/(2*sqrt(PI))
static constexpr float SH_C2_0 = 1.092548430592079f; // sqrt(15)/(2*sqrt(PI))
static constexpr float SH_C2_1 = 0.315391565252520f; // sqrt(5)/(4*sqrt(PI))
static constexpr float SH_C2_2 = 0.546274215296040f; // sqrt(15)/(4*sqrt(PI))

std::array<float, 9> SH_Basis(const glm::vec3& d) {
    std::array<float, 9> Y{};
    // Band 0
    Y[0] = SH_C0;
    // Band 1
    Y[1] = SH_C1 * d.y;
    Y[2] = SH_C1 * d.z;
    Y[3] = SH_C1 * d.x;
    // Band 2
    Y[4] = SH_C2_0 * d.x * d.y;
    Y[5] = SH_C2_0 * d.y * d.z;
    Y[6] = SH_C2_1 * (3.0f * d.z * d.z - 1.0f);
    Y[7] = SH_C2_0 * d.x * d.z;
    Y[8] = SH_C2_2 * (d.x * d.x - d.y * d.y);
    return Y;
}

glm::vec3 SH_Evaluate(const glm::vec3& normal, const SHCoefficients& coeffs) {
    auto Y = SH_Basis(normal);
    glm::vec3 result(0.0f);
    for (int i = 0; i < 9; ++i)
        result += coeffs[i] * Y[i];
    return glm::max(result, glm::vec3(0.0f));
}

// ── Cubemap face matrices ─────────────────────────────────────────────
// Standard OpenGL cubemap face directions: +X, -X, +Y, -Y, +Z, -Z

static glm::mat4 CubemapViewMatrix(int face, const glm::vec3& pos) {
    switch (face) {
        case 0: return glm::lookAt(pos, pos + glm::vec3( 1, 0, 0), glm::vec3(0,-1, 0)); // +X
        case 1: return glm::lookAt(pos, pos + glm::vec3(-1, 0, 0), glm::vec3(0,-1, 0)); // -X
        case 2: return glm::lookAt(pos, pos + glm::vec3( 0, 1, 0), glm::vec3(0, 0, 1)); // +Y
        case 3: return glm::lookAt(pos, pos + glm::vec3( 0,-1, 0), glm::vec3(0, 0,-1)); // -Y
        case 4: return glm::lookAt(pos, pos + glm::vec3( 0, 0, 1), glm::vec3(0,-1, 0)); // +Z
        case 5: return glm::lookAt(pos, pos + glm::vec3( 0, 0,-1), glm::vec3(0,-1, 0)); // -Z
        default: return glm::mat4(1.0f);
    }
}

/// Map a pixel (x,y) in face `face` to a unit direction vector on the cube.
static glm::vec3 TexelDirection(int face, int x, int y, int resolution) {
    // Map pixel center to [-1, 1]
    float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(resolution) * 2.0f - 1.0f;
    float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(resolution) * 2.0f - 1.0f;

    glm::vec3 dir;
    switch (face) {
        case 0: dir = glm::vec3( 1, -v, -u); break; // +X
        case 1: dir = glm::vec3(-1, -v,  u); break; // -X
        case 2: dir = glm::vec3( u,  1,  v); break; // +Y
        case 3: dir = glm::vec3( u, -1, -v); break; // -Y
        case 4: dir = glm::vec3( u, -v,  1); break; // +Z
        case 5: dir = glm::vec3(-u, -v, -1); break; // -Z
        default: dir = glm::vec3(0, 0, 1); break;
    }
    return glm::normalize(dir);
}

// ── LightProbe::Bake ──────────────────────────────────────────────────

void LightProbe::Bake(Scene& scene, const glm::vec3& position, uint32_t cubemapResolution) {
    uint32_t res = cubemapResolution;

    // Create FBO + color + depth renderbuffers
    GLuint fbo = 0, rboColor = 0, rboDepth = 0;
    glGenFramebuffers(1, &fbo);
    glGenRenderbuffers(1, &rboColor);
    glGenRenderbuffers(1, &rboDepth);

    glBindRenderbuffer(GL_RENDERBUFFER, rboColor);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA16F, res, res);

    glBindRenderbuffer(GL_RENDERBUFFER, rboDepth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, res, res);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rboColor);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rboDepth);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        VE_ENGINE_ERROR("LightProbe: Cubemap FBO not complete!");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteRenderbuffers(1, &rboColor);
        glDeleteRenderbuffers(1, &rboDepth);
        glDeleteFramebuffers(1, &fbo);
        return;
    }

    // 90-degree FOV perspective for cubemap faces
    glm::mat4 projection = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 500.0f);

    // Reset SH coefficients
    for (auto& c : m_Coefficients) c = glm::vec3(0.0f);
    float totalWeight = 0.0f;

    // Read buffer for pixel data
    std::vector<float> pixels(res * res * 4);

    // Save current viewport
    GLint prevViewport[4];
    glGetIntegerv(GL_VIEWPORT, prevViewport);

    for (int face = 0; face < 6; ++face) {
        glm::mat4 view = CubemapViewMatrix(face, position);
        glm::mat4 vp = projection * view;

        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, res, res);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Render the scene from this face's perspective
        // First render the sky, then the scene geometry
        scene.OnRenderSky(vp);
        scene.OnRender(vp, position);

        // Read back pixels (HDR float)
        glReadPixels(0, 0, res, res, GL_RGBA, GL_FLOAT, pixels.data());

        // Project onto SH
        for (uint32_t y = 0; y < res; ++y) {
            for (uint32_t x = 0; x < res; ++x) {
                glm::vec3 dir = TexelDirection(face, x, y, res);

                // Solid angle weight for cube-map texel
                float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(res) * 2.0f - 1.0f;
                float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(res) * 2.0f - 1.0f;
                float tmp = 1.0f + u * u + v * v;
                float weight = 4.0f / (std::sqrt(tmp) * tmp); // dw = 4 / (r^3) per face texel

                uint32_t idx = (y * res + x) * 4;
                glm::vec3 color(pixels[idx], pixels[idx + 1], pixels[idx + 2]);

                auto basis = SH_Basis(dir);
                for (int i = 0; i < 9; ++i)
                    m_Coefficients[i] += color * basis[i] * weight;

                totalWeight += weight;
            }
        }
    }

    // Normalise: the total solid angle of a sphere is 4*PI
    if (totalWeight > 0.0f) {
        float normFactor = (4.0f * glm::pi<float>()) / totalWeight;
        for (auto& c : m_Coefficients)
            c *= normFactor;
    }

    // Restore
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);

    glDeleteRenderbuffers(1, &rboColor);
    glDeleteRenderbuffers(1, &rboDepth);
    glDeleteFramebuffers(1, &fbo);

    m_Baked = true;
    VE_ENGINE_INFO("Light probe baked at ({:.1f}, {:.1f}, {:.1f}), res={}",
                   position.x, position.y, position.z, res);
}

// ── LightProbeManager ─────────────────────────────────────────────────

void LightProbeManager::Clear() {
    m_Probes.clear();
}

void LightProbeManager::AddProbe(const glm::vec3& position, float radius, const SHCoefficients& coeffs) {
    m_Probes.push_back({ position, radius, coeffs });
}

bool LightProbeManager::FindClosest(const glm::vec3& worldPos, SHCoefficients& outCoeffs) const {
    float bestDist = std::numeric_limits<float>::max();
    const ProbeEntry* best = nullptr;

    for (auto& probe : m_Probes) {
        float dist = glm::length(worldPos - probe.Position);
        if (dist <= probe.Radius && dist < bestDist) {
            bestDist = dist;
            best = &probe;
        }
    }

    if (best) {
        outCoeffs = best->Coefficients;
        return true;
    }
    return false;
}

} // namespace VE
