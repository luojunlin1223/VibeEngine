/*
 * LightProbe -- Spherical Harmonics light probe for indirect illumination.
 *
 * Stores L2 SH coefficients (9 coefficients x 3 color channels = 27 floats)
 * representing the irradiance field at a single world-space position.
 *
 * Baking renders a cubemap from the probe position and projects the result
 * onto SH basis functions.  At runtime the compact SH representation is
 * evaluated in the fragment shader to replace the constant ambient term.
 */
#pragma once

#include <glm/glm.hpp>
#include <array>
#include <vector>
#include <memory>

namespace VE {

class Scene;

// ── Spherical Harmonics helpers (L2, order 2) ─────────────────────────

/// 9 basis coefficients, each with 3 colour channels.
using SHCoefficients = std::array<glm::vec3, 9>;

/// Evaluate L2 SH basis functions for direction d (must be normalised).
/// Returns an array of 9 scalar basis values.
std::array<float, 9> SH_Basis(const glm::vec3& d);

/// Evaluate irradiance from SH coefficients for a given normal direction.
glm::vec3 SH_Evaluate(const glm::vec3& normal, const SHCoefficients& coeffs);

// ── LightProbe ────────────────────────────────────────────────────────

class LightProbe {
public:
    LightProbe() = default;

    /// Bake the probe from a cubemap rendered at `position` in the given scene.
    /// Internally: renders 6 faces, accumulates SH, normalises.
    /// `cubemapResolution` is the per-face resolution (e.g. 64).
    void Bake(Scene& scene, const glm::vec3& position, uint32_t cubemapResolution = 64);

    /// Access the baked SH coefficients.
    const SHCoefficients& GetCoefficients() const { return m_Coefficients; }
    void  SetCoefficients(const SHCoefficients& c) { m_Coefficients = c; }

    bool IsBaked() const { return m_Baked; }

private:
    SHCoefficients m_Coefficients{};
    bool m_Baked = false;
};

// ── LightProbeManager ─────────────────────────────────────────────────
// Scene-level helper: stores all baked probes and finds the closest one
// for a given world position.

class LightProbeManager {
public:
    struct ProbeEntry {
        glm::vec3      Position;
        float          Radius;
        SHCoefficients Coefficients;
    };

    void Clear();
    void AddProbe(const glm::vec3& position, float radius, const SHCoefficients& coeffs);

    /// Find the closest baked probe within influence range.
    /// Returns false if no probe is in range.
    bool FindClosest(const glm::vec3& worldPos, SHCoefficients& outCoeffs) const;

    const std::vector<ProbeEntry>& GetProbes() const { return m_Probes; }

private:
    std::vector<ProbeEntry> m_Probes;
};

} // namespace VE
