/*
 * GPULightData — Shared GPU-side light structs for rendering pipelines.
 *
 * All structs are 16-byte aligned for std430 SSBO layout.
 */
#pragma once

#include <glm/glm.hpp>

namespace VE {

// GPU-side point light (16-byte aligned for std430)
struct GPUPointLight {
    glm::vec4 PositionAndRange;     // xyz = world position, w = range
    glm::vec4 ColorAndIntensity;    // xyz = color, w = intensity
    float     _pad[4] = {};
};

// GPU-side spot light (16-byte aligned for std430)
struct GPUSpotLight {
    glm::vec4 PosAndRange;          // xyz = world position, w = range
    glm::vec4 DirAndOuterCos;       // xyz = direction, w = cos(outerAngle)
    glm::vec4 ColorAndIntensity;    // xyz = color, w = intensity
    float     InnerCos;             // cos(innerAngle)
    float     _pad[3] = {};
};

} // namespace VE
