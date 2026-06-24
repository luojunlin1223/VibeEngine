#include "VibeEngine/Scene/Scene.h"
#include "VibeEngine/Scene/Entity.h"
#include "VibeEngine/Scene/Components.h"
#include "VibeEngine/Scene/MeshLibrary.h"
#include "VibeEngine/Renderer/RenderCommand.h"
#include "VibeEngine/Renderer/ShadowMap.h"
#include "VibeEngine/Renderer/VertexArray.h"
#include "VibeEngine/Renderer/Buffer.h"
#include "VibeEngine/Renderer/Texture.h"
#include "VibeEngine/Renderer/VideoPlayer.h"
#include "VibeEngine/Scripting/ScriptEngine.h"
#include "VibeEngine/Scripting/NativeScript.h"
#include "VibeEngine/Animation/Animator.h"
#include "VibeEngine/Animation/Skeleton.h"
#include "VibeEngine/Animation/IKSolver.h"
#include "VibeEngine/Audio/AudioEngine.h"
#include "VibeEngine/Renderer/SpriteBatchRenderer.h"
#include "VibeEngine/Renderer/ParticleSystem.h"
#include "VibeEngine/Renderer/InstancedRenderer.h"
#include "VibeEngine/Renderer/Frustum.h"
#include "VibeEngine/Renderer/OcclusionCulling.h"
#include "VibeEngine/Renderer/LODSystem.h"
#include "VibeEngine/Renderer/LightProbe.h"
#include "VibeEngine/UI/UIRenderer.h"
#include "VibeEngine/UI/FontAtlas.h"
#include "VibeEngine/Terrain/Terrain.h"
#include "VibeEngine/Asset/MeshAsset.h"
#include "VibeEngine/Asset/MeshImporter.h"
#include "VibeEngine/Asset/FBXImporter.h"
#include "VibeEngine/Core/Log.h"
#include "VibeEngine/Core/Profiler.h"

#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>
#include <random>

namespace VE {

// ── Pre-built uniform name strings to avoid per-frame heap allocations ──

static const char* s_PointLightPositions[]  = { "u_PointLightPositions[0]", "u_PointLightPositions[1]", "u_PointLightPositions[2]", "u_PointLightPositions[3]", "u_PointLightPositions[4]", "u_PointLightPositions[5]", "u_PointLightPositions[6]", "u_PointLightPositions[7]" };
static const char* s_PointLightColors[]     = { "u_PointLightColors[0]", "u_PointLightColors[1]", "u_PointLightColors[2]", "u_PointLightColors[3]", "u_PointLightColors[4]", "u_PointLightColors[5]", "u_PointLightColors[6]", "u_PointLightColors[7]" };
static const char* s_PointLightIntensities[] = { "u_PointLightIntensities[0]", "u_PointLightIntensities[1]", "u_PointLightIntensities[2]", "u_PointLightIntensities[3]", "u_PointLightIntensities[4]", "u_PointLightIntensities[5]", "u_PointLightIntensities[6]", "u_PointLightIntensities[7]" };
static const char* s_PointLightRanges[]     = { "u_PointLightRanges[0]", "u_PointLightRanges[1]", "u_PointLightRanges[2]", "u_PointLightRanges[3]", "u_PointLightRanges[4]", "u_PointLightRanges[5]", "u_PointLightRanges[6]", "u_PointLightRanges[7]" };


static const char* s_SpotLightPositions[]   = { "u_SpotLightPositions[0]", "u_SpotLightPositions[1]", "u_SpotLightPositions[2]", "u_SpotLightPositions[3]" };
static const char* s_SpotLightDirections[]  = { "u_SpotLightDirections[0]", "u_SpotLightDirections[1]", "u_SpotLightDirections[2]", "u_SpotLightDirections[3]" };
static const char* s_SpotLightColors[]      = { "u_SpotLightColors[0]", "u_SpotLightColors[1]", "u_SpotLightColors[2]", "u_SpotLightColors[3]" };
static const char* s_SpotLightIntensities[] = { "u_SpotLightIntensities[0]", "u_SpotLightIntensities[1]", "u_SpotLightIntensities[2]", "u_SpotLightIntensities[3]" };
static const char* s_SpotLightRanges[]      = { "u_SpotLightRanges[0]", "u_SpotLightRanges[1]", "u_SpotLightRanges[2]", "u_SpotLightRanges[3]" };
static const char* s_SpotLightInnerCos[]    = { "u_SpotLightInnerCos[0]", "u_SpotLightInnerCos[1]", "u_SpotLightInnerCos[2]", "u_SpotLightInnerCos[3]" };
static const char* s_SpotLightOuterCos[]    = { "u_SpotLightOuterCos[0]", "u_SpotLightOuterCos[1]", "u_SpotLightOuterCos[2]", "u_SpotLightOuterCos[3]" };
static const char* s_TerrainLayers[] = { "u_Layer0", "u_Layer1", "u_Layer2", "u_Layer3" };

// ── Dummy textures for unused sampler slots ──
static GLuint s_DummyColorTexCube = 0;     // for samplerCube (reflection probes - color format)

static void EnsureDummyTextures() {
    if (s_DummyColorTexCube != 0) return;

    GLint savedActiveTexture;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &savedActiveTexture);
    glActiveTexture(GL_TEXTURE0);

    // 1x1 color cubemap for samplerCube (reflection probes - RGBA format)
    glGenTextures(1, &s_DummyColorTexCube);
    glBindTexture(GL_TEXTURE_CUBE_MAP, s_DummyColorTexCube);
    unsigned char black[4] = {0, 0, 0, 255};
    for (int i = 0; i < 6; ++i)
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, black);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    glActiveTexture(savedActiveTexture);
}

static void BindDummyReflectionProbe(const std::shared_ptr<Shader>& shader) {
    EnsureDummyTextures();

    // Bind dummy color cubemap for reflection probe (unit 13)
    glActiveTexture(GL_TEXTURE0 + 13);
    glBindTexture(GL_TEXTURE_CUBE_MAP, s_DummyColorTexCube);
    shader->SetInt("u_ReflectionProbe", 13);
    shader->SetInt("u_HasReflectionProbe", 0);
    shader->SetFloat("u_ReflectionIntensity", 0.0f);
}

static int ClampHPWaterResolution(int resolution) {
    return std::clamp(resolution, 8, 256);
}

static size_t HPWaterIndex(int x, int z, int resolution) {
    return static_cast<size_t>(z * resolution + x);
}

struct HPWaterSpectrumSample {
    float Height = 0.0f;
    float Dx = 0.0f;
    float Dz = 0.0f;
    float ChopX = 0.0f;
    float ChopZ = 0.0f;
};

static HPWaterSpectrumSample SampleHPWaterSpectrum(const HPWaterComponent& water, float localX, float localZ) {
    HPWaterSpectrumSample sample;
    if (!water.SpectrumWaves || water.SpectrumAmplitude <= 0.0f)
        return sample;

    constexpr float pi = 3.14159265358979323846f;
    constexpr float twoPi = pi * 2.0f;
    constexpr float gravity = 9.81f;
    const float windRad = water.SpectrumWindAngle * pi / 180.0f;
    const glm::vec2 wind = glm::normalize(glm::vec2(std::cos(windRad), std::sin(windRad)));
    const glm::vec2 side(-wind.y, wind.x);
    const float domain = std::max(std::max(water.WorldSizeX, water.WorldSizeZ), 1.0f);

    static const float wavelengthFactors[] = {
        0.46f, 0.32f, 0.22f, 0.155f, 0.108f, 0.076f, 0.054f, 0.038f,
        0.027f, 0.019f, 0.0135f, 0.0095f, 0.0068f, 0.0048f, 0.0034f, 0.0024f
    };
    static const float directionOffsets[] = {
        0.00f, 0.16f, -0.21f, 0.34f, -0.43f, 0.56f, -0.69f, 0.82f,
        -0.96f, 1.10f, -1.26f, 1.42f, -1.58f, 1.74f, -1.92f, 2.10f
    };
    static const float phaseOffsets[] = {
        0.0f, 1.7f, 3.1f, 4.6f, 2.4f, 5.2f, 0.9f, 3.9f,
        5.8f, 2.8f, 4.2f, 1.2f, 3.6f, 5.0f, 0.45f, 2.15f
    };

    for (int i = 0; i < 16; ++i) {
        glm::vec2 dir = glm::normalize(wind + side * directionOffsets[i]);
        const float wavelength = std::max(domain * wavelengthFactors[i], 0.25f);
        const float k = twoPi / wavelength;
        const float omega = std::sqrt(gravity * k);
        const float octave = static_cast<float>(i);
        const float swell = std::exp(-octave * 0.34f);
        const float capillaryFade = 1.0f / (1.0f + std::pow(std::max(0.0f, octave - 9.0f), 1.35f) * 0.36f);
        const float directionalEnergy = std::pow(std::max(glm::dot(dir, wind), 0.0f), 2.0f) * 0.72f + 0.28f;
        const float amplitude = water.SpectrumAmplitude * swell * capillaryFade * directionalEnergy;
        const float phase = k * (dir.x * localX + dir.y * localZ) + omega * water._OceanTime + phaseOffsets[i];
        const float s = std::sin(phase);
        const float c = std::cos(phase);

        sample.Height += s * amplitude;
        const float slopeGain = water.SpectrumNormalStrength * (1.15f + octave * 0.045f);
        sample.Dx += c * amplitude * k * dir.x * slopeGain;
        sample.Dz += c * amplitude * k * dir.y * slopeGain;
        sample.ChopX += c * amplitude * water.Choppiness * dir.x;
        sample.ChopZ += c * amplitude * water.Choppiness * dir.y;
    }

    return sample;
}

static float SampleHPWaterLocalHeight(const HPWaterComponent& water, float localX, float localZ) {
    const int n = ClampHPWaterResolution(water.Resolution);
    float dynamicHeight = 0.0f;
    if (n > 1 && water._Current.size() == static_cast<size_t>(n * n)) {
        const float u = glm::clamp(localX / std::max(water.WorldSizeX, 0.001f) + 0.5f, 0.0f, 1.0f);
        const float v = glm::clamp(localZ / std::max(water.WorldSizeZ, 0.001f) + 0.5f, 0.0f, 1.0f);
        const float fx = u * static_cast<float>(n - 1);
        const float fz = v * static_cast<float>(n - 1);
        const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, n - 1);
        const int z0 = std::clamp(static_cast<int>(std::floor(fz)), 0, n - 1);
        const int x1 = std::min(x0 + 1, n - 1);
        const int z1 = std::min(z0 + 1, n - 1);
        const float tx = fx - static_cast<float>(x0);
        const float tz = fz - static_cast<float>(z0);
        const float h00 = water._Current[HPWaterIndex(x0, z0, n)];
        const float h10 = water._Current[HPWaterIndex(x1, z0, n)];
        const float h01 = water._Current[HPWaterIndex(x0, z1, n)];
        const float h11 = water._Current[HPWaterIndex(x1, z1, n)];
        dynamicHeight = glm::mix(glm::mix(h00, h10, tx), glm::mix(h01, h11, tx), tz) * water.HeightScale;
    }

    return water.BaseHeight + dynamicHeight + SampleHPWaterSpectrum(water, localX, localZ).Height;
}

static void AddHPWaterImpulse(HPWaterComponent& water, float u, float v, float radiusWorld, float strength) {
    const int n = ClampHPWaterResolution(water.Resolution);
    if (water._Current.size() != static_cast<size_t>(n * n))
        return;

    const float cellSize = std::max(water.WorldSizeX, water.WorldSizeZ) / static_cast<float>(std::max(1, n - 1));
    const float radiusCells = std::max(radiusWorld / std::max(cellSize, 0.001f), 1.0f);
    const float centerX = glm::clamp(u, 0.0f, 1.0f) * static_cast<float>(n - 1);
    const float centerZ = glm::clamp(v, 0.0f, 1.0f) * static_cast<float>(n - 1);
    const int minX = std::max(0, static_cast<int>(std::floor(centerX - radiusCells)));
    const int maxX = std::min(n - 1, static_cast<int>(std::ceil(centerX + radiusCells)));
    const int minZ = std::max(0, static_cast<int>(std::floor(centerZ - radiusCells)));
    const int maxZ = std::min(n - 1, static_cast<int>(std::ceil(centerZ + radiusCells)));
    const float sigma = radiusCells * 0.4f;
    const float invTwoSigma2 = 1.0f / std::max(2.0f * sigma * sigma, 0.0001f);

    for (int z = minZ; z <= maxZ; ++z) {
        for (int x = minX; x <= maxX; ++x) {
            const float dx = static_cast<float>(x) - centerX;
            const float dz = static_cast<float>(z) - centerZ;
            const float dist2 = dx * dx + dz * dz;
            if (dist2 > radiusCells * radiusCells)
                continue;

            const float impulse = std::exp(-dist2 * invTwoSigma2) * strength;
            water._Current[HPWaterIndex(x, z, n)] += impulse;
        }
    }
}

static void RebuildHPWaterMesh(HPWaterComponent& water, MeshRendererComponent* meshRenderer) {
    water.Resolution = ClampHPWaterResolution(water.Resolution);
    const int n = water.Resolution;
    const size_t sampleCount = static_cast<size_t>(n * n);

    water._Current.assign(sampleCount, 0.0f);
    water._Previous.assign(sampleCount, 0.0f);
    water._Next.assign(sampleCount, 0.0f);
    water._Vertices.assign(sampleCount * 11, 0.0f);
    water._Indices.clear();
    water._Indices.reserve(static_cast<size_t>(n - 1) * static_cast<size_t>(n - 1) * 6);

    for (int z = 0; z < n - 1; ++z) {
        for (int x = 0; x < n - 1; ++x) {
            uint32_t i0 = static_cast<uint32_t>(HPWaterIndex(x, z, n));
            uint32_t i1 = static_cast<uint32_t>(HPWaterIndex(x + 1, z, n));
            uint32_t i2 = static_cast<uint32_t>(HPWaterIndex(x + 1, z + 1, n));
            uint32_t i3 = static_cast<uint32_t>(HPWaterIndex(x, z + 1, n));
            water._Indices.insert(water._Indices.end(), { i0, i2, i1, i0, i3, i2 });
        }
    }

    water._Mesh = VertexArray::Create();
    water._VertexBuffer = VertexBuffer::Create(water._Vertices.data(),
        static_cast<uint32_t>(water._Vertices.size() * sizeof(float)));
    water._VertexBuffer->SetLayout({
        { ShaderDataType::Float3, "a_Position" },
        { ShaderDataType::Float3, "a_Normal" },
        { ShaderDataType::Float3, "a_Color" },
        { ShaderDataType::Float2, "a_TexCoord" }
    });
    water._IndexBuffer = IndexBuffer::Create(water._Indices.data(),
        static_cast<uint32_t>(water._Indices.size()));
    water._Mesh->AddVertexBuffer(water._VertexBuffer);
    water._Mesh->SetIndexBuffer(water._IndexBuffer);

    if (meshRenderer) {
        meshRenderer->Mesh = water._Mesh;
        if (!meshRenderer->Mat || meshRenderer->Mat->GetName() != "Water")
            meshRenderer->Mat = MaterialLibrary::Get("Water");
        meshRenderer->CastShadows = false;
        const float spectrumExtent = water.SpectrumWaves ? std::max(water.SpectrumAmplitude, 0.0f) * 3.5f : 0.0f;
        const float chopExtent = water.SpectrumWaves ? std::max(water.SpectrumAmplitude * water.Choppiness, 0.0f) * 3.0f : 0.0f;
        meshRenderer->LocalBounds = {
            glm::vec3(-water.WorldSizeX * 0.5f - chopExtent, water.BaseHeight - water.HeightScale * 2.0f - spectrumExtent, -water.WorldSizeZ * 0.5f - chopExtent),
            glm::vec3( water.WorldSizeX * 0.5f + chopExtent, water.BaseHeight + water.HeightScale * 2.0f + spectrumExtent,  water.WorldSizeZ * 0.5f + chopExtent)
        };
    }

    water._NeedsRebuild = false;
}

static void UploadHPWaterMesh(HPWaterComponent& water) {
    const int n = ClampHPWaterResolution(water.Resolution);
    if (!water._VertexBuffer || water._Vertices.size() != static_cast<size_t>(n * n * 11))
        return;

    const float dx = water.WorldSizeX / static_cast<float>(std::max(1, n - 1));
    const float dz = water.WorldSizeZ / static_cast<float>(std::max(1, n - 1));

    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            const int xl = std::max(0, x - 1);
            const int xr = std::min(n - 1, x + 1);
            const int zd = std::max(0, z - 1);
            const int zu = std::min(n - 1, z + 1);
            const float h = water._Current[HPWaterIndex(x, z, n)];
            const float hx = water._Current[HPWaterIndex(xr, z, n)] - water._Current[HPWaterIndex(xl, z, n)];
            const float hz = water._Current[HPWaterIndex(x, zu, n)] - water._Current[HPWaterIndex(x, zd, n)];
            const float u = static_cast<float>(x) / static_cast<float>(std::max(1, n - 1));
            const float v = static_cast<float>(z) / static_cast<float>(std::max(1, n - 1));
            const float localX = (u - 0.5f) * water.WorldSizeX;
            const float localZ = (v - 0.5f) * water.WorldSizeZ;
            const HPWaterSpectrumSample spectrum = SampleHPWaterSpectrum(water, localX, localZ);
            const float dHdx =
                hx * water.HeightScale / std::max(dx * 2.0f, 0.001f) +
                spectrum.Dx;
            const float dHdz =
                hz * water.HeightScale / std::max(dz * 2.0f, 0.001f) +
                spectrum.Dz;
            const glm::vec3 normal = glm::normalize(glm::vec3(
                -dHdx,
                 1.0f,
                -dHdz));

            const size_t base = HPWaterIndex(x, z, n) * 11;
            water._Vertices[base + 0] = localX + spectrum.ChopX;
            water._Vertices[base + 1] = water.BaseHeight + h * water.HeightScale + spectrum.Height;
            water._Vertices[base + 2] = localZ + spectrum.ChopZ;
            water._Vertices[base + 3] = normal.x;
            water._Vertices[base + 4] = normal.y;
            water._Vertices[base + 5] = normal.z;
            water._Vertices[base + 6] = 1.0f;
            water._Vertices[base + 7] = 1.0f;
            water._Vertices[base + 8] = 1.0f;
            water._Vertices[base + 9] = u;
            water._Vertices[base + 10] = v;
        }
    }

    water._VertexBuffer->SetData(water._Vertices.data(),
        static_cast<uint32_t>(water._Vertices.size() * sizeof(float)));
}

static void StepHPWaterSimulation(HPWaterComponent& water, float dt) {
    const int n = ClampHPWaterResolution(water.Resolution);
    if (water._Current.size() != static_cast<size_t>(n * n))
        return;

    const float dx = water.WorldSizeX / static_cast<float>(std::max(1, n - 1));
    const float dz = water.WorldSizeZ / static_cast<float>(std::max(1, n - 1));
    const float c2dt2 = water.WaveSpeed * water.WaveSpeed * dt * dt;
    const float damping = glm::clamp(water.Damping, 0.0f, 0.98f);
    const float edgeWidth = std::max(static_cast<float>(n) * water.EdgeAbsorptionWidth, 1.0f);

    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            const size_t i = HPWaterIndex(x, z, n);
            const float current = water._Current[i];
            const float left = water._Current[HPWaterIndex(std::max(0, x - 1), z, n)];
            const float right = water._Current[HPWaterIndex(std::min(n - 1, x + 1), z, n)];
            const float down = water._Current[HPWaterIndex(x, std::max(0, z - 1), n)];
            const float up = water._Current[HPWaterIndex(x, std::min(n - 1, z + 1), n)];
            const float laplacian =
                (left + right - 2.0f * current) / std::max(dx * dx, 0.0001f) +
                (down + up - 2.0f * current) / std::max(dz * dz, 0.0001f);

            const float distToEdgeX = static_cast<float>(std::min(x, n - 1 - x));
            const float distToEdgeZ = static_cast<float>(std::min(z, n - 1 - z));
            const float edgeX = 1.0f - glm::smoothstep(0.0f, edgeWidth, distToEdgeX);
            const float edgeZ = 1.0f - glm::smoothstep(0.0f, edgeWidth, distToEdgeZ);
            float edgeAbsorption = 1.0f - (1.0f - edgeX) * (1.0f - edgeZ);
            edgeAbsorption = glm::clamp(1.0f - edgeAbsorption * edgeAbsorption, 0.0f, 1.0f);
            const float inertia = (current - water._Previous[i]) * (1.0f - damping) * edgeAbsorption;
            water._Next[i] = current + inertia + c2dt2 * laplacian;
        }
    }

    water._Previous.swap(water._Current);
    water._Current.swap(water._Next);
}

static void UpdateHPWaterComponent(HPWaterComponent& water, MeshRendererComponent* meshRenderer, float deltaTime) {
    if (!water.Enabled)
        return;

    const int desiredResolution = ClampHPWaterResolution(water.Resolution);
    if (water._NeedsRebuild || !water._Mesh || water._Current.size() != static_cast<size_t>(desiredResolution * desiredResolution))
        RebuildHPWaterMesh(water, meshRenderer);

    water._OceanTime += std::max(deltaTime, 0.0f) * std::max(water.SpectrumTimeScale, 0.0f);

    constexpr float fixedDt = 1.0f / 60.0f;
    water._Accumulator = std::min(water._Accumulator + std::max(deltaTime, 0.0f), 0.2f);
    int steps = 0;
    while (water._Accumulator >= fixedDt && steps < 8) {
        if (water.AutoImpulse) {
            water._ImpulseTimer -= fixedDt;
            if (water._ImpulseTimer <= 0.0f) {
                const float t = water._ImpulseTimer + 37.0f;
                const float u = 0.5f + 0.26f * std::sin(t * 1.91f);
                const float v = 0.5f + 0.32f * std::cos(t * 1.37f);
                AddHPWaterImpulse(water, u, v, water.ImpulseRadius, water.ImpulseStrength);
                water._ImpulseTimer = std::max(water.AutoImpulseInterval, fixedDt);
            }
        }
        StepHPWaterSimulation(water, fixedDt);
        water._Accumulator -= fixedDt;
        ++steps;
    }

    UploadHPWaterMesh(water);
    if (meshRenderer) {
        meshRenderer->Mesh = water._Mesh;
        const float spectrumExtent = water.SpectrumWaves ? std::max(water.SpectrumAmplitude, 0.0f) * 3.5f : 0.0f;
        const float chopExtent = water.SpectrumWaves ? std::max(water.SpectrumAmplitude * water.Choppiness, 0.0f) * 3.0f : 0.0f;
        meshRenderer->LocalBounds = {
            glm::vec3(-water.WorldSizeX * 0.5f - chopExtent, water.BaseHeight - water.HeightScale * 2.0f - spectrumExtent, -water.WorldSizeZ * 0.5f - chopExtent),
            glm::vec3( water.WorldSizeX * 0.5f + chopExtent, water.BaseHeight + water.HeightScale * 2.0f + spectrumExtent,  water.WorldSizeZ * 0.5f + chopExtent)
        };
    }
}

Entity Scene::CreateEntity(const std::string& name) {
    return CreateEntityWithUUID(UUID(), name);
}

Entity Scene::CreateEntityWithUUID(UUID uuid, const std::string& name) {
    Entity entity(m_Registry.create(), this);

    // Generate a unique name if the default is used
    std::string entityName = name;
    if (name == "GameObject") {
        std::ostringstream oss;
        oss << "GameObject_" << m_EntityCounter;
        entityName = oss.str();
    }
    m_EntityCounter++;

    entity.AddComponent<IDComponent>(uuid);
    entity.AddComponent<TagComponent>(entityName);
    entity.AddComponent<TransformComponent>();
    entity.AddComponent<RelationshipComponent>();

    VE_ENGINE_INFO("Entity created: {0}", entityName);
    return entity;
}

void Scene::DestroyEntity(Entity entity) {
    if (!entity.IsValid()) {
        VE_ENGINE_WARN("Scene::DestroyEntity: invalid entity, ignoring");
        return;
    }
    if (!m_Registry.valid(entity.GetHandle())) {
        VE_ENGINE_WARN("Scene::DestroyEntity: entity does not exist in registry, ignoring");
        return;
    }
    if (entity.HasComponent<TagComponent>()) {
        VE_ENGINE_INFO("Entity destroyed: {0}", entity.GetComponent<TagComponent>().Tag);
    }

    // Remove from parent's children list
    RemoveParent(entity.GetHandle());

    // Recursively destroy children
    if (entity.HasComponent<RelationshipComponent>()) {
        auto children = entity.GetComponent<RelationshipComponent>().Children; // copy
        for (auto child : children) {
            if (m_Registry.valid(child))
                DestroyEntity(Entity(child, this));
        }
    }

    // Clean up Jolt body if physics is running
    if (m_PhysicsWorld && entity.HasComponent<RigidbodyComponent>()) {
        auto& rb = entity.GetComponent<RigidbodyComponent>();
        m_PhysicsWorld->RemoveBody(rb._JoltBodyID);
    }
    m_Registry.destroy(entity.GetHandle());
}

void Scene::QueueDestroy(Entity entity) {
    entt::entity handle = entity.GetHandle();
    // Avoid duplicates in the pending list
    for (auto e : m_PendingDestroy) {
        if (e == handle) return;
    }
    m_PendingDestroy.push_back(handle);
}

void Scene::FlushDestroyQueue() {
    if (m_PendingDestroy.empty()) return;

    // Copy and clear so that DestroyEntity during flush doesn't re-enter
    auto pending = std::move(m_PendingDestroy);
    m_PendingDestroy.clear();

    for (auto e : pending) {
        if (m_Registry.valid(e))
            DestroyEntity(Entity(e, this));
    }
}

void Scene::SetParent(entt::entity child, entt::entity parent) {
    if (child == parent) {
        VE_ENGINE_WARN("Scene::SetParent: cannot parent entity to itself");
        return;
    }
    if (!m_Registry.valid(child)) {
        VE_ENGINE_WARN("Scene::SetParent: child entity is invalid");
        return;
    }
    if (!m_Registry.valid(parent)) {
        VE_ENGINE_WARN("Scene::SetParent: parent entity is invalid");
        return;
    }

    // Prevent circular: walk up from parent, if we hit child, it's circular
    entt::entity check = parent;
    while (check != entt::null) {
        if (check == child) {
            VE_ENGINE_WARN("Scene::SetParent: circular parenting detected, ignoring");
            return;
        }
        auto& rel = m_Registry.get<RelationshipComponent>(check);
        check = rel.Parent;
    }

    // Preserve world position: compute current world position before reparenting
    glm::mat4 childWorld = GetWorldTransform(child);
    glm::vec3 worldPos = glm::vec3(childWorld[3]);

    RemoveParent(child);

    auto& childRel = m_Registry.get<RelationshipComponent>(child);
    auto& parentRel = m_Registry.get<RelationshipComponent>(parent);
    childRel.Parent = parent;
    parentRel.Children.push_back(child);

    // Convert world position to new parent's local space
    glm::mat4 parentWorld = GetWorldTransform(parent);
    glm::mat4 invParent = glm::inverse(parentWorld);
    glm::vec3 localPos = glm::vec3(invParent * glm::vec4(worldPos, 1.0f));

    if (m_Registry.all_of<TransformComponent>(child)) {
        auto& tc = m_Registry.get<TransformComponent>(child);
        tc.Position = { localPos.x, localPos.y, localPos.z };
    }
}

void Scene::RemoveParent(entt::entity child) {
    if (!m_Registry.valid(child)) {
        VE_ENGINE_WARN("Scene::RemoveParent: child entity is invalid");
        return;
    }
    auto& childRel = m_Registry.get<RelationshipComponent>(child);
    if (childRel.Parent == entt::null) return;

    // Preserve world position when unparenting
    glm::mat4 childWorld = GetWorldTransform(child);
    glm::vec3 worldPos = glm::vec3(childWorld[3]);

    if (m_Registry.valid(childRel.Parent)) {
        auto& parentRel = m_Registry.get<RelationshipComponent>(childRel.Parent);
        auto& vec = parentRel.Children;
        vec.erase(std::remove(vec.begin(), vec.end(), child), vec.end());
    }
    childRel.Parent = entt::null;

    // Set local position to the old world position (now root-level)
    if (m_Registry.all_of<TransformComponent>(child)) {
        auto& tc = m_Registry.get<TransformComponent>(child);
        tc.Position = { worldPos.x, worldPos.y, worldPos.z };
    }
}

glm::mat4 Scene::GetWorldTransform(entt::entity entity) const {
    if (!m_Registry.valid(entity) || !m_Registry.all_of<TransformComponent>(entity))
        return glm::mat4(1.0f);

    auto& tc = m_Registry.get<TransformComponent>(entity);
    glm::mat4 local = glm::translate(glm::mat4(1.0f),
        glm::vec3(tc.Position[0], tc.Position[1], tc.Position[2]));
    local = glm::rotate(local, glm::radians(tc.Rotation[0]), glm::vec3(1, 0, 0));
    local = glm::rotate(local, glm::radians(tc.Rotation[1]), glm::vec3(0, 1, 0));
    local = glm::rotate(local, glm::radians(tc.Rotation[2]), glm::vec3(0, 0, 1));
    local = glm::scale(local, glm::vec3(tc.Scale[0], tc.Scale[1], tc.Scale[2]));

    if (m_Registry.all_of<RelationshipComponent>(entity)) {
        auto& rel = m_Registry.get<RelationshipComponent>(entity);
        if (rel.Parent != entt::null && m_Registry.valid(rel.Parent))
            return GetWorldTransform(rel.Parent) * local;
    }

    return local;
}

glm::vec3 Scene::GetEntityForward(entt::entity entity) const {
    if (!m_Registry.valid(entity) || !m_Registry.all_of<TransformComponent>(entity))
        return glm::normalize(glm::vec3(0.3f, 1.0f, 0.5f));

    glm::vec3 forward = glm::vec3(GetWorldTransform(entity) * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f));
    float len = glm::length(forward);
    if (len <= 0.0001f)
        return glm::normalize(glm::vec3(0.3f, 1.0f, 0.5f));
    return forward / len;
}

bool Scene::IsEntityActiveInHierarchy(entt::entity entity) const {
    if (!m_Registry.valid(entity)) return false;
    if (m_Registry.all_of<TagComponent>(entity)) {
        if (!m_Registry.get<TagComponent>(entity).Active)
            return false;
    }
    if (m_Registry.all_of<RelationshipComponent>(entity)) {
        auto& rel = m_Registry.get<RelationshipComponent>(entity);
        if (rel.Parent != entt::null && m_Registry.valid(rel.Parent))
            return IsEntityActiveInHierarchy(rel.Parent);
    }
    return true;
}

void Scene::OnUpdate(float deltaTime) {
    // Update entity count for profiler
    Profiler::SetEntityCount(static_cast<uint32_t>(m_Registry.storage<entt::entity>().size()));

    if (m_PhysicsRunning && m_PhysicsWorld) {
        PROFILE_SCOPE("Physics");
        static constexpr float PHYSICS_DT = 1.0f / 60.0f;
        m_PhysicsAccumulator += deltaTime;
        while (m_PhysicsAccumulator >= PHYSICS_DT) {
            m_PhysicsWorld->Step(PHYSICS_DT);
            m_PhysicsAccumulator -= PHYSICS_DT;
        }
        m_PhysicsWorld->SyncTransformsToScene(m_Registry);

        // Dispatch collision callbacks to scripts
        DispatchCollisionEvents();
    }

    // Run scripts after physics
    {
        PROFILE_SCOPE("Scripts");
        auto scriptView = m_Registry.view<ScriptComponent>();
        for (auto entity : scriptView) {
            if (!IsEntityActiveInHierarchy(entity)) continue;
            auto& sc = scriptView.get<ScriptComponent>(entity);
            if (sc._Instance)
                sc._Instance->OnUpdate(deltaTime);
        }
    }

    // Update animations after scripts (split into pose + IK + skinning)
    {
        auto animView = m_Registry.view<AnimatorComponent>();
        for (auto entity : animView) {
            if (!IsEntityActiveInHierarchy(entity)) continue;
            auto& ac = animView.get<AnimatorComponent>(entity);
            if (!ac._Animator) continue;

            // Check if this entity has IK targets
            auto* ikComp = m_Registry.try_get<IKComponent>(entity);
            bool hasIK = ikComp && !ikComp->Targets.empty();

            if (hasIK) {
                // Split path: sample pose, apply IK, then skin
                ac._Animator->UpdatePose(deltaTime);

                if (ac._Animator->IsPoseReady() && ac._Animator->GetMesh() &&
                    ac._Animator->GetMesh()->SkeletonRef) {
                    auto& skeleton = *ac._Animator->GetMesh()->SkeletonRef;
                    auto& localTf = ac._Animator->GetLocalTransforms();
                    auto& globalTf = ac._Animator->GetGlobalTransforms();

                    // Compute entity world transform for world-to-bone-space conversion
                    glm::mat4 entityTransform = GetWorldTransform(entity);

                    for (auto& target : ikComp->Targets) {
                        if (!target.Enabled || target.EndBoneIndex < 0) continue;
                        if (target.EndBoneIndex >= skeleton.GetBoneCount()) continue;

                        // Resolve target position from entity UUID if set
                        glm::vec3 targetPos = target.TargetPosition;
                        if (target.TargetEntityUUID != 0) {
                            auto view = m_Registry.view<IDComponent, TransformComponent>();
                            for (auto e : view) {
                                auto& id = view.get<IDComponent>(e);
                                if (static_cast<uint64_t>(id.ID) == target.TargetEntityUUID) {
                                    auto& tf = view.get<TransformComponent>(e);
                                    targetPos = glm::vec3(tf.Position[0], tf.Position[1], tf.Position[2]);
                                    break;
                                }
                            }
                        }

                        if (target.ChainLength == 2) {
                            // Two-bone IK (analytical, faster)
                            auto chain = IKSolver::BuildChain(skeleton, target.EndBoneIndex, target.ChainLength);
                            if (chain.BoneIndices.size() == 3) {
                                IKSolver::SolveTwoBone(
                                    skeleton, localTf, globalTf,
                                    chain.BoneIndices[0], chain.BoneIndices[1], chain.BoneIndices[2],
                                    targetPos, target.PoleVector, target.Weight, entityTransform);
                            }
                        } else {
                            // FABRIK (general purpose)
                            auto chain = IKSolver::BuildChain(skeleton, target.EndBoneIndex, target.ChainLength);
                            IKSolver::SolveFABRIK(
                                skeleton, localTf, globalTf, chain,
                                targetPos, target.PoleVector, target.Weight,
                                10, 0.001f, entityTransform);
                        }
                    }

                    ac._Animator->FinalizeBoneMatrices();
                }

                ac._Animator->ApplySkinning();
            } else {
                // No IK — use original combined update
                ac._Animator->Update(deltaTime);
            }
        }
    }

    // Update sprite animations
    {
        auto saView = m_Registry.view<SpriteAnimatorComponent, SpriteRendererComponent>();
        for (auto entity : saView) {
            if (!IsEntityActiveInHierarchy(entity)) continue;
            auto& sa = saView.get<SpriteAnimatorComponent>(entity);
            if (!sa._Playing) continue;

            sa._Timer += deltaTime;
            float frameDuration = 1.0f / sa.FrameRate;
            while (sa._Timer >= frameDuration) {
                sa._Timer -= frameDuration;
                sa._CurrentFrame++;
                if (sa._CurrentFrame > sa.EndFrame) {
                    if (sa.Loop)
                        sa._CurrentFrame = sa.StartFrame;
                    else {
                        sa._CurrentFrame = sa.EndFrame;
                        sa._Playing = false;
                        break;
                    }
                }
            }

            // Compute UV rect from current frame
            if (sa.Columns > 0 && sa.Rows > 0) {
                int col = sa._CurrentFrame % sa.Columns;
                int row = sa._CurrentFrame / sa.Columns;
                float uW = 1.0f / static_cast<float>(sa.Columns);
                float vH = 1.0f / static_cast<float>(sa.Rows);
                auto& sr = saView.get<SpriteRendererComponent>(entity);
                sr.UVRect = { col * uW, row * vH, uW, vH };
            }
        }
    }

    // Update nav agents
    UpdateNavAgents(deltaTime);

    // Update HPWater surfaces before rendering consumes their dynamic meshes.
    {
        auto waterView = m_Registry.view<HPWaterComponent>();
        for (auto entity : waterView) {
            if (!IsEntityActiveInHierarchy(entity)) continue;
            auto& water = waterView.get<HPWaterComponent>(entity);
            auto* mr = m_Registry.try_get<MeshRendererComponent>(entity);
            UpdateHPWaterComponent(water, mr, deltaTime);
        }
    }

    // Update particle systems
    OnUpdateParticles(deltaTime);

    // Flush deferred entity deletions (safe point — no iteration in progress)
    FlushDestroyQueue();
}

void Scene::StartPhysics() {
    if (m_PhysicsRunning) return;
    m_PhysicsWorld = std::make_unique<PhysicsWorld>();
    m_PhysicsWorld->SyncBodiesFromScene(m_Registry);
    m_PhysicsAccumulator = 0.0f;
    m_PhysicsRunning = true;
    VE_ENGINE_INFO("Physics simulation started");
}

void Scene::StopPhysics() {
    if (!m_PhysicsRunning) return;
    // Destroy all Jolt bodies
    auto view = m_Registry.view<RigidbodyComponent>();
    for (auto entity : view) {
        auto& rb = view.get<RigidbodyComponent>(entity);
        if (rb._JoltBodyID != 0xFFFFFFFF && m_PhysicsWorld) {
            m_PhysicsWorld->RemoveBody(rb._JoltBodyID);
            rb._JoltBodyID = 0xFFFFFFFF;
        }
    }
    m_PhysicsWorld.reset();
    m_PhysicsRunning = false;
    VE_ENGINE_INFO("Physics simulation stopped");
}

void Scene::StartScripts() {
    ScriptEngine::SetActiveScene(this);

    auto view = m_Registry.view<ScriptComponent>();
    for (auto entity : view) {
        auto& sc = view.get<ScriptComponent>(entity);
        if (sc.ClassName.empty()) continue;
        if (sc._Instance) continue; // already running

        sc._Instance = ScriptEngine::CreateInstance(sc.ClassName);
        if (sc._Instance) {
            auto& id = m_Registry.get<IDComponent>(entity);
            sc._Instance->m_EntityID = static_cast<uint64_t>(id.ID);
            // Apply stored property values before OnCreate
            ScriptEngine::ApplyPropertiesToInstance(sc._Instance, sc.ClassName, sc.Properties);
            sc._Instance->OnCreate();
        }
    }
    VE_ENGINE_INFO("Scripts started");
}

void Scene::StopScripts() {
    auto view = m_Registry.view<ScriptComponent>();
    for (auto entity : view) {
        auto& sc = view.get<ScriptComponent>(entity);
        if (sc._Instance) {
            // Read back property values before destroying
            ScriptEngine::ReadPropertiesFromInstance(sc._Instance, sc.ClassName, sc.Properties);
            sc._Instance->OnDestroy();
            ScriptEngine::DestroyInstance(sc._Instance);
            sc._Instance = nullptr;
        }
    }
    VE_ENGINE_INFO("Scripts stopped");
}

entt::entity Scene::FindEntityByBodyID(uint32_t bodyID) const {
    auto view = m_Registry.view<RigidbodyComponent>();
    for (auto entity : view) {
        auto& rb = view.get<RigidbodyComponent>(entity);
        if (rb._JoltBodyID == bodyID)
            return entity;
    }
    return entt::null;
}

void Scene::DispatchCollisionEvents() {
    if (!m_PhysicsWorld) return;

    const auto& events = m_PhysicsWorld->GetCollisionEvents();
    if (events.empty()) return;

    for (const auto& ev : events) {
        entt::entity entityA = FindEntityByBodyID(ev.BodyA);
        entt::entity entityB = FindEntityByBodyID(ev.BodyB);

        uint64_t idA = 0, idB = 0;
        if (entityA != entt::null && m_Registry.all_of<IDComponent>(entityA))
            idA = static_cast<uint64_t>(m_Registry.get<IDComponent>(entityA).ID);
        if (entityB != entt::null && m_Registry.all_of<IDComponent>(entityB))
            idB = static_cast<uint64_t>(m_Registry.get<IDComponent>(entityB).ID);

        // Dispatch to scripts on both entities
        auto dispatch = [&](entt::entity entity, uint64_t otherID, bool isEnter,
                            const glm::vec3& cp, const glm::vec3& cn) {
            if (entity == entt::null) return;
            auto* sc = m_Registry.try_get<ScriptComponent>(entity);
            if (!sc || !sc->_Instance) return;

            if (isEnter) {
                ScriptCollisionInfo info;
                info.OtherEntityID = otherID;
                info.ContactPoint[0] = cp.x; info.ContactPoint[1] = cp.y; info.ContactPoint[2] = cp.z;
                info.ContactNormal[0] = cn.x; info.ContactNormal[1] = cn.y; info.ContactNormal[2] = cn.z;
                sc->_Instance->OnCollisionEnter(info);
            } else {
                sc->_Instance->OnCollisionExit(otherID);
            }
        };

        dispatch(entityA, idB, ev.IsEnter, ev.ContactPoint, ev.ContactNormal);
        dispatch(entityB, idA, ev.IsEnter, ev.ContactPoint, -ev.ContactNormal);
    }

    m_PhysicsWorld->ClearCollisionEvents();
}

void Scene::StartAnimations() {
    auto view = m_Registry.view<AnimatorComponent, MeshRendererComponent>();
    for (auto entity : view) {
        auto& ac = view.get<AnimatorComponent>(entity);
        auto& mr = view.get<MeshRendererComponent>(entity);

        // Need a skinned MeshAsset — look it up from MeshSourcePath
        if (mr.MeshSourcePath.empty()) continue;
        auto meshAsset = MeshImporter::GetOrLoad(mr.MeshSourcePath);
        if (!meshAsset || !meshAsset->IsSkinned()) continue;

        ac._Animator = std::make_shared<Animator>();
        ac._Animator->SetTarget(meshAsset);

        // If an external animation source is specified, load clips from it
        if (!ac.AnimationSourcePath.empty() && meshAsset->SkeletonRef) {
            auto externalClips = FBXImporter::ImportAnimations(ac.AnimationSourcePath, meshAsset->SkeletonRef);
            if (!externalClips.empty())
                ac._Animator->SetClips(std::move(externalClips));
        }

        // Configure state machine if enabled
        if (ac.UseStateMachine && !ac.States.empty()) {
            if (ac.States.empty() || ac.DefaultState >= (int)ac.States.size()) continue;
            auto& sm = ac._Animator->GetStateMachine();
            for (auto& s : ac.States) sm.AddState(s);
            for (auto& t : ac.Transitions) sm.AddTransition(t);
            for (auto& p : ac.Parameters) sm.AddParameter(p);
            sm.SetDefaultState(ac.DefaultState);
            sm.Reset();
            ac._Animator->SetUseStateMachine(true);
            ac._Animator->Play(ac.States[ac.DefaultState].ClipIndex,
                               ac.States[ac.DefaultState].Loop,
                               ac.States[ac.DefaultState].Speed);
        } else {
            int clipCount = ac._Animator->GetClipCount();
            if (ac.PlayOnStart && ac.ClipIndex < clipCount)
                ac._Animator->Play(ac.ClipIndex, ac.Loop, ac.Speed);
        }
    }
    VE_ENGINE_INFO("Animations started");
}

void Scene::StopAnimations() {
    auto view = m_Registry.view<AnimatorComponent>();
    for (auto entity : view) {
        auto& ac = view.get<AnimatorComponent>(entity);
        if (ac._Animator) {
            ac._Animator->Stop();
            ac._Animator.reset();
        }
    }
    VE_ENGINE_INFO("Animations stopped");
}

void Scene::StartAudio() {
    if (!AudioEngine::IsInitialized()) return;

    auto view = m_Registry.view<AudioSourceComponent>();
    for (auto entity : view) {
        auto& as = view.get<AudioSourceComponent>(entity);
        if (as.ClipPath.empty()) continue;

        if (as.PlayOnAwake) {
            as._SoundHandle = AudioEngine::Play(as.ClipPath, as.Volume, as.Pitch, as.Loop);
            if (as._SoundHandle != 0 && as.Spatial) {
                AudioEngine::SetSoundSpatial(as._SoundHandle, true);
                AudioEngine::SetSoundMinMaxDistance(as._SoundHandle, as.MinDistance, as.MaxDistance);
                if (m_Registry.all_of<TransformComponent>(entity)) {
                    auto& tc = m_Registry.get<TransformComponent>(entity);
                    AudioEngine::SetSoundPosition(as._SoundHandle, tc.Position.data());
                }
            }
        }
    }
    VE_ENGINE_INFO("Audio started");
}

void Scene::StopAudio() {
    auto view = m_Registry.view<AudioSourceComponent>();
    for (auto entity : view) {
        auto& as = view.get<AudioSourceComponent>(entity);
        if (as._SoundHandle != 0) {
            AudioEngine::Stop(as._SoundHandle);
            as._SoundHandle = 0;
        }
    }
    VE_ENGINE_INFO("Audio stopped");
}

void Scene::UpdateAudio(const float listenerPos[3], const float listenerForward[3], const float listenerUp[3]) {
    if (!AudioEngine::IsInitialized()) return;

    // Update listener position
    AudioEngine::SetListenerPosition(listenerPos, listenerForward, listenerUp);

    // Update spatial sound positions
    auto view = m_Registry.view<AudioSourceComponent, TransformComponent>();
    for (auto entity : view) {
        auto& as = view.get<AudioSourceComponent>(entity);
        if (as.Spatial && as._SoundHandle != 0) {
            auto& tc = view.get<TransformComponent>(entity);
            AudioEngine::SetSoundPosition(as._SoundHandle, tc.Position.data());
        }
    }
}

glm::mat4 Scene::ComputeCameraView(const glm::mat4& worldTransform) {
    glm::vec3 position = glm::vec3(worldTransform[3]);
    glm::mat3 rotMat   = glm::mat3(worldTransform); // upper-left 3x3 includes rotation+scale
    // VibeEngine camera objects use local +Z as their forward direction.
    glm::vec3 forward = glm::normalize(rotMat * glm::vec3(0, 0, 1));
    glm::vec3 up      = glm::normalize(rotMat * glm::vec3(0, 1,  0));
    return glm::lookAt(position, position + forward, up);
}

glm::mat4 Scene::ComputeCameraProjection(int projType, float fov, float size,
                                          float nearClip, float farClip, float aspectRatio) {
    if (projType == 0) { // Perspective
        return glm::perspective(glm::radians(fov), aspectRatio, nearClip, farClip);
    } else { // Orthographic
        float halfH = size;
        float halfW = halfH * aspectRatio;
        return glm::ortho(-halfW, halfW, -halfH, halfH, nearClip, farClip);
    }
}

static glm::mat4 ComputeModelMatrix(const TransformComponent& tc) {
    glm::mat4 model = glm::translate(glm::mat4(1.0f),
        glm::vec3(tc.Position[0], tc.Position[1], tc.Position[2]));
    model = glm::rotate(model, glm::radians(tc.Rotation[0]), glm::vec3(1, 0, 0));
    model = glm::rotate(model, glm::radians(tc.Rotation[1]), glm::vec3(0, 1, 0));
    model = glm::rotate(model, glm::radians(tc.Rotation[2]), glm::vec3(0, 0, 1));
    model = glm::scale(model, glm::vec3(tc.Scale[0], tc.Scale[1], tc.Scale[2]));
    return model;
}

// ── Reflection Probes ───────────────────────────────────────────────

void Scene::BakeReflectionProbes() {
    auto probeView = m_Registry.view<TransformComponent, ReflectionProbeComponent>();
    for (auto entity : probeView) {
        BakeReflectionProbe(entity);
    }
}

void Scene::BakeReflectionProbe(entt::entity probeEntity) {
    if (!m_Registry.valid(probeEntity)) return;
    if (!m_Registry.all_of<TransformComponent, ReflectionProbeComponent>(probeEntity)) return;

    auto& tc = m_Registry.get<TransformComponent>(probeEntity);
    auto& rpc = m_Registry.get<ReflectionProbeComponent>(probeEntity);

    // Create or recreate probe if resolution changed
    if (!rpc._Probe || rpc._Probe->GetResolution() != rpc.Resolution) {
        rpc._Probe = std::make_shared<ReflectionProbe>(rpc.Resolution);
    }

    glm::mat4 worldMat = GetWorldTransform(probeEntity);
    glm::vec3 position = glm::vec3(worldMat[3]);

    rpc._Probe->Capture(*this, position);
    rpc._IsBaked = rpc._Probe->IsBaked();
}

// ── Navigation ──────────────────────────────────────────────────────

void Scene::BakeNavGrid(float cellSize, float worldSize) {
    m_NavGrid = std::make_unique<NavGrid>(
        NavGridBuilder::BuildFromScene(*this, cellSize, worldSize));
}

void Scene::UpdateNavAgents(float deltaTime) {
    if (!m_NavGrid) return;

    auto view = m_Registry.view<TransformComponent, NavAgentComponent>();
    for (auto entity : view) {
        if (!IsEntityActiveInHierarchy(entity)) continue;
        auto& tc = view.get<TransformComponent>(entity);
        auto& nav = view.get<NavAgentComponent>(entity);

        if (!nav._HasTarget || nav._Path.empty()) continue;
        if (nav._PathIndex >= static_cast<int>(nav._Path.size())) {
            nav._HasTarget = false;
            continue;
        }

        float targetX = nav._Path[nav._PathIndex][0];
        float targetZ = nav._Path[nav._PathIndex][1];
        float dx = targetX - tc.Position[0];
        float dz = targetZ - tc.Position[2];
        float dist = std::sqrt(dx * dx + dz * dz);

        if (dist <= nav.StoppingDist) {
            nav._PathIndex++;
            if (nav._PathIndex >= static_cast<int>(nav._Path.size())) {
                nav._HasTarget = false;
            }
            continue;
        }

        float speed = nav.Speed * deltaTime;
        if (speed > dist) speed = dist;

        tc.Position[0] += (dx / dist) * speed;
        tc.Position[2] += (dz / dist) * speed;

        // Face movement direction
        tc.Rotation[1] = std::atan2(dx, dz) * 57.2958f;
    }
}

void Scene::OnRenderSky(const glm::mat4& skyViewProjection) {
    if (!m_PipelineSettings.SkyEnabled) return;

    auto skyShader = MeshLibrary::GetSkyShader();
    auto skyMesh   = MeshLibrary::GetSkySphere();
    if (!skyShader || !skyMesh) return;

    auto& sky = m_PipelineSettings;

    RenderCommand::SetDepthWrite(false);
    RenderCommand::SetDepthFunc(RendererAPI::DepthFunc::LessEqual);

    skyShader->Bind();
    skyShader->ApplyRenderState(); // Cull Front, ZWrite Off, ZTest LEqual
    skyShader->SetMat4("u_MVP", skyViewProjection);
    skyShader->SetVec3("u_TopColor",
        glm::vec3(sky.SkyTopColor[0], sky.SkyTopColor[1], sky.SkyTopColor[2]));
    skyShader->SetVec3("u_BottomColor",
        glm::vec3(sky.SkyBottomColor[0], sky.SkyBottomColor[1], sky.SkyBottomColor[2]));

    if (sky.SkyTexture) {
        sky.SkyTexture->Bind(0);
        skyShader->SetInt("u_Texture", 0);
        skyShader->SetInt("u_UseTexture", 1);
    } else {
        skyShader->SetInt("u_UseTexture", 0);
    }

    RenderCommand::DrawIndexed(skyMesh);

    // Restore default state for subsequent passes
    RenderCommand::SetDepthWrite(true);
    RenderCommand::SetDepthFunc(RendererAPI::DepthFunc::Less);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
}


// ── Deferred Rendering ─────────────────────────────────────────────

void Scene::OnRenderDeferred(const glm::mat4& viewProjection,
                              const glm::mat4& cameraView, const glm::mat4& cameraProjection,
                              const glm::vec3& cameraPos,
                              float nearClip, float farClip,
                              uint32_t viewportWidth, uint32_t viewportHeight) {
    // ── Initialize or resize the deferred renderer ──
    const uint64_t renderDiagnosticsFrame = m_RenderDiagnostics.FrameIndex + 1;
    m_RenderDiagnostics = {};
    m_RenderDiagnostics.FrameIndex = renderDiagnosticsFrame;
    m_RenderDiagnostics.ViewportWidth = viewportWidth;
    m_RenderDiagnostics.ViewportHeight = viewportHeight;

    if (!m_DeferredRenderer.IsInitialized()) {
        m_DeferredRenderer.Init(viewportWidth, viewportHeight);
    } else if (m_DeferredRenderer.GetWidth() != viewportWidth ||
               m_DeferredRenderer.GetHeight() != viewportHeight) {
        m_DeferredRenderer.Resize(viewportWidth, viewportHeight);
    }
    m_RenderDiagnostics.DeferredInitialized = m_DeferredRenderer.IsInitialized();
    m_RenderDiagnostics.HPWaterGBufferInitialized = m_DeferredRenderer.HasHPWaterGBuffer();
    m_RenderDiagnostics.HPWaterGBufferAttachmentCount =
        static_cast<uint32_t>(m_DeferredRenderer.GetHPWaterGBufferAttachmentCount());
    m_RenderDiagnostics.HPWaterGBuffer0 = m_DeferredRenderer.GetHPWaterGBufferTexture(0);
    m_RenderDiagnostics.HPWaterGBuffer1 = m_DeferredRenderer.GetHPWaterGBufferTexture(1);
    m_RenderDiagnostics.HPWaterGBuffer2 = m_DeferredRenderer.GetHPWaterGBufferTexture(2);
    m_RenderDiagnostics.HPWaterGBufferDepth = m_DeferredRenderer.GetHPWaterDepthTexture();
    m_RenderDiagnostics.HPWaterMaskTexture = m_DeferredRenderer.GetHPWaterMaskTexture();
    m_RenderDiagnostics.HPWaterMaskWidth = m_DeferredRenderer.GetWidth();
    m_RenderDiagnostics.HPWaterMaskHeight = m_DeferredRenderer.GetHeight();
    m_RenderDiagnostics.HPWaterCompositeTexture = m_DeferredRenderer.GetHPWaterCompositeTexture();
    m_RenderDiagnostics.HPWaterRefractionDataTexture = m_DeferredRenderer.GetHPWaterRefractionDataTexture();
    m_RenderDiagnostics.HPWaterRefractionMetaTexture = m_DeferredRenderer.GetHPWaterRefractionMetaTexture();
    m_RenderDiagnostics.HPWaterDepthPyramidTexture = m_DeferredRenderer.GetHPWaterDepthPyramidTexture();
    m_RenderDiagnostics.HPWaterDepthPyramidMipCount = m_DeferredRenderer.GetHPWaterDepthPyramidMipCount();
    m_RenderDiagnostics.HPWaterDepthPyramidWidth = m_DeferredRenderer.GetWidth();
    m_RenderDiagnostics.HPWaterDepthPyramidHeight = m_DeferredRenderer.GetHeight();
    m_RenderDiagnostics.HPWaterDepthMergedToSceneDepth =
        m_DeferredRenderer.IsHPWaterDepthMergedToSceneDepth();
    m_RenderDiagnostics.HPWaterNormalMergedToSceneGBuffer =
        m_DeferredRenderer.IsHPWaterNormalMergedToSceneGBuffer();
    m_RenderDiagnostics.HPWaterStencilMarkedInSceneDepth =
        m_DeferredRenderer.IsHPWaterStencilMarkedInSceneDepth();
    m_RenderDiagnostics.HPWaterStencilRef = m_DeferredRenderer.GetHPWaterStencilRef();
    m_RenderDiagnostics.HPWaterPreintegratedFGDLUTValid = m_DeferredRenderer.IsHPWaterFGDLUTValid();
    m_RenderDiagnostics.HPWaterPreintegratedFGDLUTTexture = m_DeferredRenderer.GetHPWaterFGDLUTTexture();
    m_RenderDiagnostics.HPWaterPreintegratedFGDLUTResolution = m_DeferredRenderer.GetHPWaterFGDLUTResolution();
    m_RenderDiagnostics.HPWaterForwardScatterMipEnabled = m_DeferredRenderer.IsHPWaterSceneColorMipValid();
    m_RenderDiagnostics.HPWaterForwardScatterMipCount = m_DeferredRenderer.GetHPWaterSceneColorMipCount();
    m_RenderDiagnostics.HPWaterVolumeColorTexture = m_DeferredRenderer.GetHPWaterVolumeTexture(0);
    m_RenderDiagnostics.HPWaterVolumeTransmittanceTexture = m_DeferredRenderer.GetHPWaterVolumeTexture(1);
    m_RenderDiagnostics.HPWaterVolumeDepthTexture = m_DeferredRenderer.GetHPWaterVolumeTexture(2);
    m_RenderDiagnostics.HPWaterVolumeWidth = m_DeferredRenderer.GetHPWaterVolumeWidth();
    m_RenderDiagnostics.HPWaterVolumeHeight = m_DeferredRenderer.GetHPWaterVolumeHeight();
    m_RenderDiagnostics.HPWaterVolumeTemporalNeighborhoodClampEnabled =
        m_DeferredRenderer.IsHPWaterVolumeTemporalNeighborhoodClampEnabled();
    m_RenderDiagnostics.HPWaterVolumeTemporalMotionReprojectionEnabled =
        m_DeferredRenderer.IsHPWaterVolumeTemporalMotionReprojectionEnabled();
    m_RenderDiagnostics.HPWaterVolumeExplicitMotionVectorEnabled =
        m_DeferredRenderer.IsHPWaterVolumeExplicitMotionVectorEnabled();
    m_RenderDiagnostics.HPWaterVolumeExponentialIntegrationEnabled =
        m_DeferredRenderer.IsHPWaterVolumeExponentialIntegrationEnabled();
    m_RenderDiagnostics.HPWaterVolumeShadowSamplingEnabled =
        m_DeferredRenderer.IsHPWaterVolumeShadowSamplingEnabled();
    m_RenderDiagnostics.HPWaterSurfaceShadowSamplingEnabled =
        m_DeferredRenderer.IsHPWaterSurfaceShadowSamplingEnabled();
    m_RenderDiagnostics.HPWaterShadowCascadeDitherEnabled =
        m_DeferredRenderer.IsHPWaterShadowCascadeDitherEnabled();
    m_RenderDiagnostics.HPWaterVolumeShadowParamsEnabled =
        m_DeferredRenderer.IsHPWaterVolumeShadowParamsEnabled();
    m_RenderDiagnostics.HPWaterVolumeShadowSoftness =
        m_DeferredRenderer.GetHPWaterVolumeShadowSoftness();
    m_RenderDiagnostics.HPWaterVolumeShadowMinFilterSize =
        m_DeferredRenderer.GetHPWaterVolumeShadowMinFilterSize();
    m_RenderDiagnostics.HPWaterVolumeShadowBlockerSamples =
        m_DeferredRenderer.GetHPWaterVolumeShadowBlockerSamples();
    m_RenderDiagnostics.HPWaterVolumeShadowFilterSamples =
        m_DeferredRenderer.GetHPWaterVolumeShadowFilterSamples();
    m_RenderDiagnostics.HPWaterVolumeSampleCount =
        m_DeferredRenderer.GetHPWaterVolumeSampleCount();
    m_RenderDiagnostics.HPWaterVolumeMotionVectorTexture =
        m_DeferredRenderer.GetHPWaterVolumeMotionVectorTexture();
    m_RenderDiagnostics.HPWaterVolumeTemporalNeighborhoodClampStrength =
        m_DeferredRenderer.GetHPWaterVolumeTemporalNeighborhoodClampStrength();
    m_RenderDiagnostics.HPWaterVolumeHistoryValid = m_DeferredRenderer.HasHPWaterVolumeHistory();
    m_RenderDiagnostics.HPWaterVolumeHistoryColorTexture = m_DeferredRenderer.GetHPWaterVolumeHistoryTexture(0);
    m_RenderDiagnostics.HPWaterVolumeHistoryTransmittanceTexture = m_DeferredRenderer.GetHPWaterVolumeHistoryTexture(1);
    m_RenderDiagnostics.HPWaterVolumeHistoryDepthTexture = m_DeferredRenderer.GetHPWaterVolumeHistoryTexture(2);
    m_RenderDiagnostics.HPWaterVolumeFilterIterations = m_DeferredRenderer.GetHPWaterVolumeFilterIterations();
    m_RenderDiagnostics.HPWaterVolumeFilteredColorTexture = m_DeferredRenderer.GetHPWaterVolumeFilteredTexture(0);
    m_RenderDiagnostics.HPWaterVolumeFilteredTransmittanceTexture = m_DeferredRenderer.GetHPWaterVolumeFilteredTexture(1);
    m_RenderDiagnostics.HPWaterVolumeFilteredDepthTexture = m_DeferredRenderer.GetHPWaterVolumeFilteredTexture(2);
    m_RenderDiagnostics.HPWaterVolumeUpsampledColorTexture = m_DeferredRenderer.GetHPWaterVolumeUpsampledTexture(0);
    m_RenderDiagnostics.HPWaterVolumeUpsampledTransmittanceTexture = m_DeferredRenderer.GetHPWaterVolumeUpsampledTexture(1);
    m_RenderDiagnostics.HPWaterVolumeUpsampledDepthTexture = m_DeferredRenderer.GetHPWaterVolumeUpsampledTexture(2);
    m_RenderDiagnostics.HPWaterVolumeUpsampledWidth = m_DeferredRenderer.GetWidth();
    m_RenderDiagnostics.HPWaterVolumeUpsampledHeight = m_DeferredRenderer.GetHeight();
    m_RenderDiagnostics.HPWaterCausticTexture = m_DeferredRenderer.GetHPWaterCausticTexture();
    m_RenderDiagnostics.HPWaterCausticValid = m_DeferredRenderer.IsHPWaterCausticValid();
    m_RenderDiagnostics.HPWaterCausticComputeTexture =
        m_DeferredRenderer.GetHPWaterCausticComputeIrradianceTexture();
    m_RenderDiagnostics.HPWaterCausticComputeWidth =
        m_DeferredRenderer.GetHPWaterCausticComputeWidth();
    m_RenderDiagnostics.HPWaterCausticComputeHeight =
        m_DeferredRenderer.GetHPWaterCausticComputeHeight();
    m_RenderDiagnostics.HPWaterCausticComputeValid =
        m_DeferredRenderer.IsHPWaterCausticComputeIrradianceValid();
    m_RenderDiagnostics.HPWaterCausticComputeRan =
        m_DeferredRenderer.DidRunHPWaterCausticComputeIrradiance();
    m_RenderDiagnostics.HPWaterCausticComputeAtomicEnabled =
        m_DeferredRenderer.IsHPWaterCausticComputeAtomicEnabled();
    m_RenderDiagnostics.HPWaterCausticComputeAtomicTexture =
        m_DeferredRenderer.GetHPWaterCausticComputeAtomicTexture();
    m_RenderDiagnostics.HPWaterCausticShadowDepthConsumed =
        m_DeferredRenderer.IsHPWaterCausticShadowDepthConsumed();
    m_RenderDiagnostics.HPWaterCausticRGBReceiverProjectionEnabled =
        m_DeferredRenderer.IsHPWaterCausticRGBReceiverProjectionEnabled();
    m_RenderDiagnostics.HPWaterCausticExponentialLightStepsEnabled =
        m_DeferredRenderer.IsHPWaterCausticExponentialLightStepsEnabled();
    m_RenderDiagnostics.HPWaterCausticFrameDitherEnabled =
        m_DeferredRenderer.IsHPWaterCausticFrameDitherEnabled();
    m_RenderDiagnostics.HPWaterCausticAtlasReceiverOutputEnabled =
        m_DeferredRenderer.IsHPWaterCausticAtlasReceiverOutputEnabled();
    m_RenderDiagnostics.HPWaterCausticCascadeBlendEnabled =
        m_DeferredRenderer.IsHPWaterCausticCascadeBlendEnabled();
    m_RenderDiagnostics.HPWaterCausticAtlasEdgeFilterEnabled =
        m_DeferredRenderer.IsHPWaterCausticAtlasEdgeFilterEnabled();
    m_RenderDiagnostics.HPWaterCausticSpectralWeightingEnabled =
        m_DeferredRenderer.IsHPWaterCausticSpectralWeightingEnabled();
    m_RenderDiagnostics.HPWaterCausticFilteredTexture = m_DeferredRenderer.GetHPWaterCausticFilteredTexture();
    m_RenderDiagnostics.HPWaterCausticFilteredValid = m_DeferredRenderer.IsHPWaterCausticFilteredValid();
    m_RenderDiagnostics.HPWaterCausticFilterIterations = m_DeferredRenderer.GetHPWaterCausticFilterIterations();
    m_RenderDiagnostics.HPWaterCausticFilterComputeParityEnabled =
        m_DeferredRenderer.IsHPWaterCausticFilterComputeParityEnabled();
    m_RenderDiagnostics.HPWaterCausticFilterLDSHaloEnabled =
        m_DeferredRenderer.IsHPWaterCausticFilterLDSHaloEnabled();
    m_RenderDiagnostics.HPWaterCausticAtlasTexture = m_DeferredRenderer.GetHPWaterCausticAtlasTexture();
    m_RenderDiagnostics.HPWaterCausticAtlasDepthTexture = m_DeferredRenderer.GetHPWaterCausticAtlasDepthTexture();
    m_RenderDiagnostics.HPWaterCausticAtlasValid = m_DeferredRenderer.IsHPWaterCausticAtlasValid();
    m_RenderDiagnostics.HPWaterCausticAtlasTileResolution = m_DeferredRenderer.GetHPWaterCausticAtlasTileResolution();
    m_RenderDiagnostics.HPWaterCausticAtlasWidth = m_DeferredRenderer.GetHPWaterCausticAtlasWidth();
    m_RenderDiagnostics.HPWaterCausticAtlasHeight = m_DeferredRenderer.GetHPWaterCausticAtlasHeight();
    m_RenderDiagnostics.HPWaterCausticAtlasCascades = m_DeferredRenderer.GetHPWaterCausticAtlasCascadeCount();
    m_RenderDiagnostics.HPWaterCausticAtlasConsumed = m_DeferredRenderer.IsHPWaterCausticAtlasConsumed();

    auto gbufferShader = m_DeferredRenderer.GetGBufferShader();
    if (!gbufferShader) {
        VE_ENGINE_ERROR("DeferredRenderer: No G-buffer shader — cannot render");
        return;
    }

    // ── Gather lights (same as forward path) ──
    glm::vec3 lightDir = glm::normalize(glm::vec3(0.3f, 1.0f, 0.5f));
    glm::vec3 lightColor(1.0f);
    float lightIntensity = 1.0f;
    {
        auto lightView = m_Registry.view<DirectionalLightComponent>();
        for (auto lightEntity : lightView) {
            if (!IsEntityActiveInHierarchy(lightEntity)) continue;
            auto& dl = lightView.get<DirectionalLightComponent>(lightEntity);
            lightDir = GetEntityForward(lightEntity);
            lightColor = glm::vec3(dl.Color[0], dl.Color[1], dl.Color[2]);
            lightIntensity = dl.Intensity;
            break;
        }
    }

    static constexpr int MAX_POINT_LIGHTS = 8;
    int numPointLights = 0;
    std::array<glm::vec3, MAX_POINT_LIGHTS> pointPositions = {};
    std::array<glm::vec3, MAX_POINT_LIGHTS> pointColors = {};
    std::array<float, MAX_POINT_LIGHTS> pointIntensities = {};
    std::array<float, MAX_POINT_LIGHTS> pointRanges = {};
    {
        auto plView = m_Registry.view<TransformComponent, PointLightComponent>();
        for (auto plEntity : plView) {
            if (numPointLights >= MAX_POINT_LIGHTS) break;
            if (!IsEntityActiveInHierarchy(plEntity)) continue;
            auto [tc, pl] = plView.get<TransformComponent, PointLightComponent>(plEntity);
            glm::mat4 worldMat = GetWorldTransform(plEntity);
            pointPositions[numPointLights]   = glm::vec3(worldMat[3]);
            pointColors[numPointLights]      = glm::vec3(pl.Color[0], pl.Color[1], pl.Color[2]);
            pointIntensities[numPointLights] = pl.Intensity;
            pointRanges[numPointLights]      = pl.Range;
            numPointLights++;
        }
    }

    static constexpr int MAX_SPOT_LIGHTS = 4;
    int numSpotLights = 0;
    std::array<glm::vec3, MAX_SPOT_LIGHTS> spotPositions = {};
    std::array<glm::vec3, MAX_SPOT_LIGHTS> spotDirections = {};
    std::array<glm::vec3, MAX_SPOT_LIGHTS> spotColors = {};
    std::array<float, MAX_SPOT_LIGHTS> spotIntensities = {};
    std::array<float, MAX_SPOT_LIGHTS> spotRanges = {};
    std::array<float, MAX_SPOT_LIGHTS> spotInnerCos = {};
    std::array<float, MAX_SPOT_LIGHTS> spotOuterCos = {};
    {
        auto slView = m_Registry.view<TransformComponent, SpotLightComponent>();
        for (auto slEntity : slView) {
            if (numSpotLights >= MAX_SPOT_LIGHTS) break;
            if (!IsEntityActiveInHierarchy(slEntity)) continue;
            auto [tc, sl] = slView.get<TransformComponent, SpotLightComponent>(slEntity);
            glm::mat4 worldMat = GetWorldTransform(slEntity);
            spotPositions[numSpotLights]   = glm::vec3(worldMat[3]);
            glm::vec3 localDir = glm::normalize(glm::vec3(sl.Direction[0], sl.Direction[1], sl.Direction[2]));
            spotDirections[numSpotLights]  = glm::normalize(glm::mat3(worldMat) * localDir);
            spotColors[numSpotLights]      = glm::vec3(sl.Color[0], sl.Color[1], sl.Color[2]);
            spotIntensities[numSpotLights] = sl.Intensity;
            spotRanges[numSpotLights]      = sl.Range;
            spotInnerCos[numSpotLights]    = std::cos(glm::radians(sl.InnerAngle));
            spotOuterCos[numSpotLights]    = std::cos(glm::radians(sl.OuterAngle));
            numSpotLights++;
        }
    }

    auto& ps = m_PipelineSettings;
    auto colorToVec3 = [](const std::array<float, 3>& color) {
        return glm::vec3(color[0], color[1], color[2]);
    };
    glm::vec3 indirectSkyColor = ps.SkyEnabled ? colorToVec3(ps.SkyTopColor) : glm::vec3(0.03f);
    glm::vec3 indirectGroundColor = ps.SkyEnabled ? colorToVec3(ps.SkyBottomColor) : glm::vec3(0.03f);
    glm::vec3 indirectTint = colorToVec3(ps.IndirectTint);

    // Lambda to set lighting uniforms on the deferred lighting shader
    auto setLightingUniforms = [&](const std::shared_ptr<Shader>& shader) {
        shader->SetVec3("u_LightDir", lightDir);
        shader->SetVec3("u_LightColor", lightColor);
        shader->SetFloat("u_LightIntensity", lightIntensity);
        shader->SetVec3("u_ViewPos", cameraPos);
        shader->SetInt("u_IndirectLightingEnabled", ps.IndirectLightingEnabled ? 1 : 0);
        shader->SetVec3("u_IndirectSkyColor", indirectSkyColor);
        shader->SetVec3("u_IndirectGroundColor", indirectGroundColor);
        shader->SetVec3("u_IndirectTint", indirectTint);
        shader->SetFloat("u_IndirectDiffuseIntensity", ps.IndirectDiffuseIntensity);
        shader->SetFloat("u_SkyReflectionIntensity", ps.SkyReflectionIntensity);

        BindDummyReflectionProbe(shader);

        shader->SetInt("u_NumPointLights", numPointLights);
        for (int i = 0; i < numPointLights; ++i) {
            shader->SetVec3(s_PointLightPositions[i], pointPositions[i]);
            shader->SetVec3(s_PointLightColors[i], pointColors[i]);
            shader->SetFloat(s_PointLightIntensities[i], pointIntensities[i]);
            shader->SetFloat(s_PointLightRanges[i], pointRanges[i]);
        }

        shader->SetInt("u_NumSpotLights", numSpotLights);
        for (int i = 0; i < numSpotLights; ++i) {
            shader->SetVec3(s_SpotLightPositions[i], spotPositions[i]);
            shader->SetVec3(s_SpotLightDirections[i], spotDirections[i]);
            shader->SetVec3(s_SpotLightColors[i], spotColors[i]);
            shader->SetFloat(s_SpotLightIntensities[i], spotIntensities[i]);
            shader->SetFloat(s_SpotLightRanges[i], spotRanges[i]);
            shader->SetFloat(s_SpotLightInnerCos[i], spotInnerCos[i]);
            shader->SetFloat(s_SpotLightOuterCos[i], spotOuterCos[i]);
        }
    };

    // ── Shadow settings helper ──
    ShadowSettings shadowSettings;
    shadowSettings.Enabled           = ps.ShadowsEnabled;
    shadowSettings.Resolution        = ps.ShadowResolution;
    shadowSettings.MaxDistance        = ps.ShadowMaxDistance;
    shadowSettings.SplitLambda       = ps.ShadowSplitLambda;
    shadowSettings.DepthBias         = ps.ShadowDepthBias;
    shadowSettings.NormalBias        = ps.ShadowNormalBias;
    shadowSettings.PCFQuality        = ps.ShadowPCFQuality;
    shadowSettings.CascadeBlendWidth = ps.ShadowCascadeBlendWidth;

    // ── Shadow Depth Pass (before geometry) ──────────────────────────
    if (ps.ShadowsEnabled) {
        if (!m_ShadowMap.IsInitialized())
            m_ShadowMap.Init(ps.ShadowResolution);
        else if (m_ShadowMap.GetResolution() != ps.ShadowResolution)
            m_ShadowMap.Resize(ps.ShadowResolution);

        // Unbind shadow texture from sampler before writing to it (avoid read-write conflict)
        glActiveTexture(GL_TEXTURE0 + ShadowMap::TEXTURE_UNIT);
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

        m_ShadowMap.Update(cameraView, cameraProjection, lightDir, nearClip, farClip, shadowSettings);

        auto depthShader = m_ShadowMap.GetDepthShader();
        if (depthShader) {
            auto shadowMeshView = m_Registry.view<TransformComponent, MeshRendererComponent>();
            for (int c = 0; c < ShadowMap::NUM_CASCADES; ++c) {
                m_ShadowMap.BeginCascadePass(c);
                glm::mat4 lightVP = m_ShadowMap.GetLightViewProjection(c);
                depthShader->Bind();

                for (auto entityID : shadowMeshView) {
                    if (!IsEntityActiveInHierarchy(entityID)) continue;
                    auto& mr = shadowMeshView.get<MeshRendererComponent>(entityID);
                    if (!mr.Mesh) continue;

                    if (!mr.CastShadows) {
                        continue;
                    }
                    // Skip transparent objects
                    if (mr.Mat && (mr.Mat->IsTransparent() || mr.Color[3] < 0.999f)) {
                        continue;
                    }

                    std::shared_ptr<VertexArray> shadowVAO = mr.Mesh;
                    if (m_Registry.all_of<AnimatorComponent>(entityID)) {
                        auto& ac = m_Registry.get<AnimatorComponent>(entityID);
                        if (ac._Animator && ac._Animator->GetSkinnedVAO())
                            shadowVAO = ac._Animator->GetSkinnedVAO();
                    }

                    glm::mat4 model = GetWorldTransform(entityID);
                    depthShader->SetMat4("u_LightMVP", lightVP * model);
                    RenderCommand::DrawIndexed(shadowVAO);
                }
                m_ShadowMap.EndCascadePass();
            }
        } else {
            VE_ENGINE_WARN("[CSM] Depth shader is null! Shadow depth pass skipped.");
        }

        // Restore viewport for subsequent passes
        RenderCommand::SetViewport(0, 0, viewportWidth, viewportHeight);
    }

    // ── Frustum culling setup ──
    Frustum frustum(viewProjection);
    static const AABB s_UnitAABB = { glm::vec3(-0.5f), glm::vec3(0.5f) };
    auto& stats = const_cast<RenderStats&>(RenderCommand::GetStats());

    // ── Phase 1: G-Buffer Geometry Pass ──
    m_DeferredRenderer.BeginGeometryPass();

    // Collect transparent entities for later forward pass
    struct VisibleEntity {
        entt::entity ID;
        glm::mat4    Model;
        float        DistSq;
    };
    std::vector<VisibleEntity> transparentEntities;
    std::vector<VisibleEntity> hpWaterEntities;
    float hpWaterRefractionStrength = 0.0f;
    float hpWaterWaterDispersionStrength = 0.0f;
    float hpWaterMaxRefractionCrossDistance = 0.1f;
    float hpWaterRefractionThicknessOffset = 0.01f;
    uint32_t hpWaterRefractionSampleCount = 4;
    bool hpWaterRefractionJitter = false;
    float hpWaterEnvironmentReflectionIntensity = 0.0f;
    float hpWaterIndirectLightStrength = 0.0f;
    float hpWaterMacroScatterStrength = 0.0f;
    float hpWaterVolumeShadowSoftness = 2.0f;
    float hpWaterVolumeShadowMinFilterSize = 1.0f;
    int hpWaterVolumeShadowBlockerSamples = 8;
    int hpWaterVolumeShadowFilterSamples = 16;
    float hpWaterVolumeTemporalBlendFactor = 0.0f;
    bool hpWaterVolumeSpatialFilterEnabled = false;
    int hpWaterVolumeSpatialFilterIterations = 1;
    bool hpWaterVolumeMotionVectorsEnabled = false;
    float hpWaterVolumeMotionVectorVelocityScale = 0.0f;
    bool hpWaterVolumeTemporalDepthRejection = false;
    float hpWaterVolumeTemporalDepthThreshold = 0.0f;
    bool hpWaterVolumeSpatialDepthAware = false;
    float hpWaterVolumeSpatialDepthSensitivity = 0.0f;
    float hpWaterThinSSSStrength = 0.0f;
    float hpWaterBacklitTransmissionStrength = 0.0f;
    float hpWaterForwardScatterStrength = 0.0f;
    float hpWaterForwardScatterBlurDensity = 0.0f;
    float hpWaterMultiScatterScale = 0.0f;
    float hpWaterPhaseG = -0.95f;
    float hpWaterSpecularFGDStrength = 0.0f;
    float hpWaterGGXEnergyCompensation = 0.0f;
    bool hpWaterCausticsEnabled = false;
    float hpWaterCausticStrength = 0.0f;
    float hpWaterCausticScale = 12.0f;
    float hpWaterCausticDepthFade = 20.0f;
    float hpWaterCausticTransmittanceStrength = 1.0f;
    float hpWaterCausticLeakReduction = 0.65f;
    float hpWaterCausticShadowAlphaClipThreshold = 0.0f;
    float hpWaterCausticScatterBoost = 0.35f;
    bool hpWaterCausticRGBDispersion = false;
    float hpWaterCausticDispersionStrength = 0.0f;
    bool hpWaterCausticAtlasEnabled = false;
    uint32_t hpWaterCausticAtlasResolution = 512;
    uint32_t hpWaterCausticAtlasDrawn = 0;
    bool hpWaterCausticFilterEnabled = false;
    float hpWaterCausticFilterRadius = 1.35f;
    float hpWaterCausticFilterDepthSigma = 0.0025f;
    float hpWaterCausticFilterLuminanceWeight = 0.5f;
    int hpWaterCausticFilterIterations = 1;
    float hpWaterCausticVolumeStrength = 0.0f;
    bool hpWaterFluidEnabled = false;
    uint32_t hpWaterFluidResolution = 128;
    float hpWaterFluidWaveSpeed = 1.0f;
    float hpWaterFluidDamping = 0.018f;
    float hpWaterFluidSourceU = -1.0f;
    float hpWaterFluidSourceV = -1.0f;
    float hpWaterFluidSourceIntensity = 0.0f;
    float hpWaterFluidSourceRadius = 5.0f;
    glm::vec3 hpWaterFluidBoxCenter(0.0f);
    glm::vec3 hpWaterFluidBoxSize(1.0f);
    bool hpWaterFluidObstaclesEnabled = false;
    bool hpWaterFluidStartFrameBake = false;
    uint32_t hpWaterFluidWaterLayerMask = (1u << 4u);
    float hpWaterFluidObstaclePadding = 1.0f;
    float hpWaterFluidObstacleHeightRange = 4.0f;
    float hpWaterFluidSurfaceY = 0.0f;
    struct HPWaterFluidObstacleCandidate {
        glm::vec3 Min;
        glm::vec3 Max;
    };
    struct HPWaterFluidCaptureDraw {
        entt::entity ID;
        std::shared_ptr<VertexArray> Mesh;
        glm::mat4 Model;
    };
    std::vector<HPWaterFluidObstacleCandidate> hpWaterFluidObstacleCandidates;
    std::vector<HPWaterFluidCaptureDraw> hpWaterFluidSceneCaptureDraws;

    auto view = m_Registry.view<TransformComponent, MeshRendererComponent>();
    transparentEntities.reserve(view.size_hint() / 4);
    hpWaterEntities.reserve(4);
    hpWaterFluidObstacleCandidates.reserve(view.size_hint());
    hpWaterFluidSceneCaptureDraws.reserve(view.size_hint());

    auto hpWaterLayerView = m_Registry.view<HPWaterComponent, TagComponent>();
    for (auto entityID : hpWaterLayerView) {
        if (!IsEntityActiveInHierarchy(entityID))
            continue;
        const auto& water = hpWaterLayerView.get<HPWaterComponent>(entityID);
        const auto& tag = hpWaterLayerView.get<TagComponent>(entityID);
        if (!water.Enabled || !water.FluidDynamicsEnabled)
            continue;
        if (tag.Layer >= 0 && tag.Layer < 32)
            hpWaterFluidWaterLayerMask = (1u << static_cast<uint32_t>(tag.Layer));
        break;
    }

    for (auto entityID : view) {
        if (!IsEntityActiveInHierarchy(entityID)) continue;
        auto [tc, mr] = view.get<TransformComponent, MeshRendererComponent>(entityID);
        const bool isHPWater = m_Registry.any_of<HPWaterComponent>(entityID);
        m_RenderDiagnostics.MeshRendererEntities++;
        if (isHPWater) {
            m_RenderDiagnostics.HPWaterEntities++;
            if (mr.Mesh)
                m_RenderDiagnostics.HPWaterWithMesh++;
        }

        if (!mr.Mesh || !mr.Mat) continue;

        glm::mat4 model = GetWorldTransform(entityID);
        glm::vec3 worldMin( std::numeric_limits<float>::max());
        glm::vec3 worldMax(-std::numeric_limits<float>::max());

        // ── Frustum cull ──
        {
            if (!mr.LocalBounds.Valid()) {
                bool found = false;
                for (int idx = 0; idx < MeshLibrary::GetMeshCount(); ++idx) {
                    if (mr.Mesh == MeshLibrary::GetMeshByIndex(idx)) {
                        mr.LocalBounds = MeshLibrary::GetMeshAABB(idx);
                        found = true;
                        break;
                    }
                }
                if (!found) mr.LocalBounds = s_UnitAABB;
            }

            const AABB& localAABB = mr.LocalBounds;
            for (int i = 0; i < 8; ++i) {
                glm::vec3 corner(
                    (i & 1) ? localAABB.Max.x : localAABB.Min.x,
                    (i & 2) ? localAABB.Max.y : localAABB.Min.y,
                    (i & 4) ? localAABB.Max.z : localAABB.Min.z
                );
                glm::vec3 worldCorner = glm::vec3(model * glm::vec4(corner, 1.0f));
                worldMin = glm::min(worldMin, worldCorner);
                worldMax = glm::max(worldMax, worldCorner);
            }

            const auto* tag = m_Registry.try_get<TagComponent>(entityID);
            const uint32_t entityLayerMask = (tag && tag->Layer >= 0 && tag->Layer < 32)
                ? (1u << static_cast<uint32_t>(tag->Layer))
                : 0u;
            const bool isWaterLayerForFluid = (entityLayerMask & hpWaterFluidWaterLayerMask) != 0u;
            const bool isTransparentForFluid = !isHPWater && (mr.Mat->IsTransparent() || mr.Color[3] < 0.999f);
            if (!isHPWater && !isWaterLayerForFluid && !isTransparentForFluid) {
                hpWaterFluidObstacleCandidates.push_back({ worldMin, worldMax });
                hpWaterFluidSceneCaptureDraws.push_back({ entityID, mr.Mesh, model });
            }

            if (!frustum.TestAABB(worldMin, worldMax)) {
                stats.CulledObjects++;
                m_RenderDiagnostics.FrustumCulled++;
                if (isHPWater)
                    m_RenderDiagnostics.HPWaterCulled++;
                continue;
            }
            stats.VisibleObjects++;
        }

        // ── LOD selection ──
        if (m_Registry.all_of<LODGroupComponent>(entityID)) {
            auto& lodGroup = m_Registry.get<LODGroupComponent>(entityID);
            if (!lodGroup.Levels.empty()) {
                glm::vec3 worldPos = glm::vec3(model[3]);
                float dist = glm::length(worldPos - cameraPos);
                int lodIndex = SelectLOD(lodGroup, dist);
                if (lodIndex < 0) {
                    stats.CulledObjects++;
                    stats.VisibleObjects--;
                    continue;
                }
                lodGroup._ActiveLOD = lodIndex;
                auto& level = lodGroup.Levels[lodIndex];
                if (level.Mesh) mr.Mesh = level.Mesh;
            }
        }

        // ── Separate transparent from opaque ──
        bool isTransparent = !isHPWater && (mr.Mat->IsTransparent() || mr.Color[3] < 0.999f);
        if (isTransparent) {
            glm::vec3 worldPos = glm::vec3(model[3]);
            float distSq = glm::dot(worldPos - cameraPos, worldPos - cameraPos);
            transparentEntities.push_back({ entityID, model, distSq });
            m_RenderDiagnostics.TransparentQueued++;
            continue;
        }
        if (isHPWater) {
            hpWaterEntities.push_back({ entityID, model, 0.0f });
            if (auto* water = m_Registry.try_get<HPWaterComponent>(entityID)) {
                hpWaterRefractionStrength = std::max(hpWaterRefractionStrength, water->RefractionStrength);
                hpWaterWaterDispersionStrength = std::max(
                    hpWaterWaterDispersionStrength,
                    std::clamp(water->WaterDispersionStrength, 0.0f, 2.0f));
                hpWaterMaxRefractionCrossDistance = std::max(
                    hpWaterMaxRefractionCrossDistance, water->MaxRefractionCrossDistance);
                hpWaterRefractionThicknessOffset = std::max(
                    hpWaterRefractionThicknessOffset, water->RefractionThicknessOffset);
                hpWaterRefractionSampleCount = std::max(
                    hpWaterRefractionSampleCount,
                    static_cast<uint32_t>(std::clamp(water->RefractionSampleCount, 4, 64)));
                hpWaterRefractionJitter = hpWaterRefractionJitter || water->RefractionJitter;
                hpWaterEnvironmentReflectionIntensity = std::max(
                    hpWaterEnvironmentReflectionIntensity,
                    water->EnvironmentReflectionIntensity);
                hpWaterIndirectLightStrength = std::max(
                    hpWaterIndirectLightStrength,
                    std::clamp(water->IndirectLightStrength, 0.0f, 4.0f));
                hpWaterMacroScatterStrength = std::max(
                    hpWaterMacroScatterStrength,
                    water->MacroScatterStrength);
                hpWaterVolumeShadowSoftness = std::max(
                    hpWaterVolumeShadowSoftness,
                    std::clamp(water->VolumeShadowSoftness, 0.0f, 10.0f));
                hpWaterVolumeShadowMinFilterSize = std::max(
                    hpWaterVolumeShadowMinFilterSize,
                    std::clamp(water->VolumeShadowMinFilterSize, 0.0f, 8.0f));
                hpWaterVolumeShadowBlockerSamples = std::max(
                    hpWaterVolumeShadowBlockerSamples,
                    std::clamp(water->VolumeShadowBlockerSamples, 0, 16));
                hpWaterVolumeShadowFilterSamples = std::max(
                    hpWaterVolumeShadowFilterSamples,
                    std::clamp(water->VolumeShadowFilterSamples, 1, 16));
                hpWaterVolumeTemporalBlendFactor = std::max(
                    hpWaterVolumeTemporalBlendFactor,
                    std::clamp(water->VolumeTemporalBlendFactor, 0.0f, 0.98f));
                hpWaterVolumeSpatialFilterEnabled =
                    hpWaterVolumeSpatialFilterEnabled || water->VolumeSpatialFilterEnabled;
                hpWaterVolumeSpatialFilterIterations = std::max(
                    hpWaterVolumeSpatialFilterIterations,
                    std::clamp(water->VolumeSpatialFilterIterations, 1, 3));
                hpWaterVolumeMotionVectorsEnabled =
                    hpWaterVolumeMotionVectorsEnabled || water->VolumeMotionVectorsEnabled;
                hpWaterVolumeMotionVectorVelocityScale = std::max(
                    hpWaterVolumeMotionVectorVelocityScale,
                    std::clamp(water->VolumeMotionVectorVelocityScale, 0.0f, 10.0f));
                hpWaterVolumeTemporalDepthRejection =
                    hpWaterVolumeTemporalDepthRejection || water->VolumeTemporalDepthRejection;
                hpWaterVolumeTemporalDepthThreshold = std::max(
                    hpWaterVolumeTemporalDepthThreshold,
                    std::clamp(water->VolumeTemporalDepthThreshold, 0.0001f, 10.0f));
                hpWaterVolumeSpatialDepthAware =
                    hpWaterVolumeSpatialDepthAware || water->VolumeSpatialDepthAware;
                hpWaterVolumeSpatialDepthSensitivity = std::max(
                    hpWaterVolumeSpatialDepthSensitivity,
                    std::clamp(water->VolumeSpatialDepthSensitivity, 0.0f, 1000.0f));
                hpWaterThinSSSStrength = std::max(
                    hpWaterThinSSSStrength,
                    water->ThinSSSStrength);
                hpWaterBacklitTransmissionStrength = std::max(
                    hpWaterBacklitTransmissionStrength,
                    water->BacklitTransmissionStrength);
                hpWaterForwardScatterStrength = std::max(
                    hpWaterForwardScatterStrength,
                    water->ForwardScatterStrength);
                hpWaterForwardScatterBlurDensity = std::max(
                    hpWaterForwardScatterBlurDensity,
                    water->ForwardScatterBlurDensity);
                hpWaterMultiScatterScale = std::max(
                    hpWaterMultiScatterScale,
                    water->MultiScatterScale);
                hpWaterPhaseG = std::max(
                    hpWaterPhaseG,
                    std::clamp(water->PhaseG, -0.95f, 0.95f));
                hpWaterSpecularFGDStrength = std::max(
                    hpWaterSpecularFGDStrength,
                    water->SpecularFGDStrength);
                hpWaterGGXEnergyCompensation = std::max(
                    hpWaterGGXEnergyCompensation,
                    water->GGXEnergyCompensation);
                hpWaterCausticsEnabled = hpWaterCausticsEnabled || water->CausticsEnabled;
                hpWaterCausticStrength = std::max(hpWaterCausticStrength, water->CausticStrength);
                hpWaterCausticScale = std::max(hpWaterCausticScale, water->CausticScale);
                hpWaterCausticDepthFade = std::max(hpWaterCausticDepthFade, water->CausticDepthFade);
                hpWaterCausticTransmittanceStrength = std::max(
                    hpWaterCausticTransmittanceStrength, water->CausticTransmittanceStrength);
                hpWaterCausticLeakReduction = std::max(
                    hpWaterCausticLeakReduction, water->CausticLeakReduction);
                hpWaterCausticShadowAlphaClipThreshold = std::max(
                    hpWaterCausticShadowAlphaClipThreshold,
                    std::clamp(water->CausticShadowAlphaClipThreshold, 0.0f, 1.0f));
                hpWaterCausticScatterBoost = std::max(
                    hpWaterCausticScatterBoost, water->CausticScatterBoost);
                hpWaterCausticRGBDispersion = hpWaterCausticRGBDispersion || water->CausticRGBDispersion;
                hpWaterCausticDispersionStrength = std::max(
                    hpWaterCausticDispersionStrength, water->CausticDispersionStrength);
                hpWaterCausticAtlasEnabled = hpWaterCausticAtlasEnabled ||
                    (water->CausticsEnabled && water->CausticLightSpaceAtlasEnabled);
                hpWaterCausticAtlasResolution = std::max(
                    hpWaterCausticAtlasResolution,
                    static_cast<uint32_t>(std::clamp(water->CausticAtlasResolution, 128, 2048)));
                hpWaterCausticFilterEnabled = hpWaterCausticFilterEnabled || water->CausticFilterEnabled;
                hpWaterCausticFilterRadius = std::max(hpWaterCausticFilterRadius, water->CausticFilterRadius);
                hpWaterCausticFilterDepthSigma = std::max(hpWaterCausticFilterDepthSigma, water->CausticFilterDepthSigma);
                hpWaterCausticFilterLuminanceWeight = std::max(
                    hpWaterCausticFilterLuminanceWeight,
                    std::clamp(water->CausticFilterLuminanceWeight, 0.0f, 128.0f));
                hpWaterCausticFilterIterations = std::max(
                    hpWaterCausticFilterIterations,
                    std::clamp(water->CausticFilterIterations, 1, 2));
                hpWaterCausticVolumeStrength = std::max(
                    hpWaterCausticVolumeStrength, water->CausticVolumeStrength);
                if (!hpWaterFluidEnabled && water->FluidDynamicsEnabled) {
                    hpWaterFluidEnabled = true;
                    hpWaterFluidResolution = static_cast<uint32_t>(
                        std::clamp(water->FluidResolution, 16, 1024));
                    hpWaterFluidWaveSpeed = water->FluidWaveSpeed;
                    hpWaterFluidDamping = water->FluidDamping;
                    hpWaterFluidSourceRadius = water->FluidImpulseRadius;
                    hpWaterFluidSourceIntensity = water->AutoImpulse ? water->FluidImpulseStrength : 0.0f;
                    hpWaterFluidObstaclesEnabled = water->FluidObstaclesEnabled;
                    hpWaterFluidStartFrameBake = water->FluidStartFrameBake;
                    hpWaterFluidObstaclePadding = std::max(water->FluidObstaclePadding, 0.0f);
                    hpWaterFluidObstacleHeightRange = std::max(water->FluidObstacleHeightRange, 0.0f);
                    if (water->AutoImpulse) {
                        const float t = water->_OceanTime + static_cast<float>(m_RenderDiagnostics.FrameIndex) * 0.017f;
                        hpWaterFluidSourceU = 0.5f + 0.26f * std::sin(t * 1.91f);
                        hpWaterFluidSourceV = 0.5f + 0.32f * std::cos(t * 1.37f);
                    }
                    hpWaterFluidBoxCenter = glm::vec3(model[3]);
                    const float sx = glm::length(glm::vec3(model[0]));
                    const float sy = glm::length(glm::vec3(model[1]));
                    const float sz = glm::length(glm::vec3(model[2]));
                    hpWaterFluidSurfaceY = hpWaterFluidBoxCenter.y + water->BaseHeight * sy;
                    hpWaterFluidBoxSize = glm::vec3(
                        std::max(water->WorldSizeX * sx, 0.001f),
                        std::max((std::abs(water->HeightScale) * 4.0f + std::abs(water->BaseHeight) + 1.0f) * sy, 0.001f),
                        std::max(water->WorldSizeZ * sz, 0.001f));
                }
            }
            m_RenderDiagnostics.HPWaterQueued++;
            continue;
        }

        m_RenderDiagnostics.OpaqueSubmitted++;

        // ── Render opaque entity to G-buffer ──
        std::shared_ptr<VertexArray> drawVAO = mr.Mesh;
        auto* ac = m_Registry.try_get<AnimatorComponent>(entityID);
        bool hasAnimator = ac && ac->_Animator && ac->_Animator->GetSkinnedVAO();
        if (hasAnimator) drawVAO = ac->_Animator->GetSkinnedVAO();
        glm::mat4 mvp = viewProjection * model;
        glm::vec4 entityColor(mr.Color[0], mr.Color[1], mr.Color[2], mr.Color[3]);

        // Use the G-buffer shader for all opaque geometry
        gbufferShader->Bind();
        gbufferShader->SetMat4("u_MVP", mvp);
        gbufferShader->SetMat4("u_Model", model);
        gbufferShader->SetVec4("u_EntityColor", entityColor);

        // Set PBR material defaults
        gbufferShader->SetFloat("u_Metallic", 0.0f);
        gbufferShader->SetFloat("u_Roughness", 0.5f);
        gbufferShader->SetFloat("u_AO", 1.0f);
        gbufferShader->SetFloat("u_BumpScale", 1.0f);
        gbufferShader->SetFloat("u_OcclusionStrength", 1.0f);
        gbufferShader->SetVec4("u_EmissionColor", glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        gbufferShader->SetFloat("u_Cutoff", 0.0f);
        gbufferShader->SetInt("u_IsWaterSurface", 0);
        gbufferShader->SetVec3("u_WaterAbsorptionColor", glm::vec3(0.0f));
        gbufferShader->SetVec3("u_WaterFoamColor", glm::vec3(1.0f));
        gbufferShader->SetFloat("u_WaterFoamIntensity", 0.0f);
        gbufferShader->SetFloat("u_WaterHeightScale", 1.0f);
        gbufferShader->SetFloat("u_WaterBaseHeight", 0.0f);
        gbufferShader->SetInt("u_UseTexture", 0);
        gbufferShader->SetInt("u_HasMainTex", 0);
        gbufferShader->SetInt("u_HasMetallicGlossMap", 0);
        gbufferShader->SetInt("u_HasBumpMap", 0);
        gbufferShader->SetInt("u_HasOcclusionMap", 0);
        gbufferShader->SetInt("u_HasEmissionMap", 0);

        // Bind the original material to extract texture bindings
        // (Material::Bind sets uniforms on whatever shader is currently bound)
        if (mr.Mat->IsLit()) {
            // Set PBR properties from material
            for (auto& prop : mr.Mat->GetProperties()) {
                if (prop.Type == MaterialPropertyType::Float)
                    gbufferShader->SetFloat(prop.Name, prop.FloatValue);
                else if (prop.Type == MaterialPropertyType::Int)
                    gbufferShader->SetInt(prop.Name, prop.IntValue);
                else if (prop.Type == MaterialPropertyType::Vec3)
                    gbufferShader->SetVec3(prop.Name, prop.Vec3Value);
                else if (prop.Type == MaterialPropertyType::Vec4)
                    gbufferShader->SetVec4(prop.Name, prop.Vec4Value);
                else if (prop.Type == MaterialPropertyType::Texture2D && prop.TextureRef) {
                    // Find texture unit by name
                    int unit = 0;
                    if (prop.Name == "u_MainTex") unit = 0;
                    else if (prop.Name == "u_MetallicGlossMap") unit = 1;
                    else if (prop.Name == "u_BumpMap") unit = 2;
                    else if (prop.Name == "u_OcclusionMap") unit = 3;
                    else if (prop.Name == "u_EmissionMap") unit = 4;
                    prop.TextureRef->Bind(unit);
                    gbufferShader->SetInt(prop.Name, unit);
                    if (!prop.FlagName.empty())
                        gbufferShader->SetInt(prop.FlagName, 1);
                }
            }
        }

        // Apply per-entity material overrides
        for (const auto& ov : mr.MaterialOverrides) {
            switch (ov.Type) {
                case MaterialPropertyType::Float:
                    gbufferShader->SetFloat(ov.Name, ov.FloatValue); break;
                case MaterialPropertyType::Int:
                    gbufferShader->SetInt(ov.Name, ov.IntValue); break;
                case MaterialPropertyType::Vec3:
                    gbufferShader->SetVec3(ov.Name, ov.Vec3Value); break;
                case MaterialPropertyType::Vec4:
                    gbufferShader->SetVec4(ov.Name, ov.Vec4Value); break;
                case MaterialPropertyType::Texture2D:
                    if (ov.TextureRef) {
                        ov.TextureRef->Bind(0);
                        gbufferShader->SetInt(ov.Name, 0);
                        if (!ov.FlagName.empty())
                            gbufferShader->SetInt(ov.FlagName, 1);
                        else if (ov.Name == "u_MainTex")
                            gbufferShader->SetInt("u_HasMainTex", 1);
                        gbufferShader->SetInt("u_UseTexture", 1);
                    }
                    break;
            }
        }

        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        RenderCommand::DrawIndexed(drawVAO);
    }

    m_DeferredRenderer.EndGeometryPass();

    if (hpWaterFluidEnabled) {
        if (hpWaterFluidObstaclesEnabled) {
            const float obstacleHalfY =
                std::abs(hpWaterFluidSurfaceY - hpWaterFluidBoxCenter.y) +
                hpWaterFluidObstacleHeightRange +
                hpWaterFluidObstaclePadding +
                0.5f;
            hpWaterFluidBoxSize.y = std::max(hpWaterFluidBoxSize.y, obstacleHalfY * 2.0f);
        }

        const float waterMinY = hpWaterFluidBoxCenter.y - hpWaterFluidBoxSize.y * 0.5f;
        const float invY = hpWaterFluidBoxSize.y > 0.0001f ? 1.0f / hpWaterFluidBoxSize.y : 0.0f;
        const auto normalizeHeight = [&](float worldY) -> float {
            return std::clamp((worldY - waterMinY) * invY, 0.0f, 1.0f);
        };
        const float normalizedWaterHeight = normalizeHeight(hpWaterFluidSurfaceY);

        const float captureNear = 0.0f;
        const float captureFar = std::max(hpWaterFluidBoxSize.y, 0.001f);
        const glm::vec3 captureEye =
            hpWaterFluidBoxCenter + glm::vec3(0.0f, hpWaterFluidBoxSize.y * 0.5f, 0.0f);
        const glm::vec3 captureTarget =
            hpWaterFluidBoxCenter - glm::vec3(0.0f, hpWaterFluidBoxSize.y * 0.5f, 0.0f);
        const glm::mat4 topDownView = glm::lookAt(captureEye, captureTarget, glm::vec3(0.0f, 0.0f, 1.0f));
        const glm::mat4 topDownProjection = glm::ortho(
            -hpWaterFluidBoxSize.x * 0.5f,
             hpWaterFluidBoxSize.x * 0.5f,
            -hpWaterFluidBoxSize.z * 0.5f,
             hpWaterFluidBoxSize.z * 0.5f,
             captureNear,
             captureFar);
        const glm::mat4 topDownVP = topDownProjection * topDownView;

        bool gpuHeightCaptureValid = false;
        bool gpuWaterHeightCaptured = false;
        bool displacedWaterHeightCaptured = false;
        bool sceneGeometryHeightCaptured = false;
        uint32_t gpuWaterCaptureDraws = 0;
        uint32_t gpuSceneCaptureDraws = 0;
        auto heightCaptureShader = m_DeferredRenderer.GetHPWaterFluidHeightCaptureShader();
        m_DeferredRenderer.BeginHPWaterFluidHeightCaptureFrame(hpWaterFluidStartFrameBake);
        const bool reuseHeightCapture =
            hpWaterFluidStartFrameBake &&
            m_DeferredRenderer.CanReuseHPWaterFluidHeightCapture(hpWaterFluidResolution,
                                                                 hpWaterFluidBoxCenter,
                                                                 hpWaterFluidBoxSize);
        if (reuseHeightCapture) {
            m_DeferredRenderer.MarkHPWaterFluidHeightCaptureCacheReused();
            gpuHeightCaptureValid = m_DeferredRenderer.IsHPWaterFluidHeightCaptureValid();
        }
        if (heightCaptureShader && !reuseHeightCapture) {
            if (m_DeferredRenderer.BeginHPWaterFluidWaterHeightCapture(hpWaterFluidResolution,
                                                                       hpWaterFluidBoxCenter,
                                                                       hpWaterFluidBoxSize)) {
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                heightCaptureShader->Bind();
                heightCaptureShader->SetVec3("u_BoxCenter", hpWaterFluidBoxCenter);
                heightCaptureShader->SetVec3("u_BoxSize", glm::max(hpWaterFluidBoxSize, glm::vec3(0.001f)));
                heightCaptureShader->SetFloat("u_ForceHeight", -1.0f);
                for (const auto& ve : hpWaterEntities) {
                    auto* mr = m_Registry.try_get<MeshRendererComponent>(ve.ID);
                    if (!mr || !mr->Mesh)
                        continue;
                    heightCaptureShader->SetMat4("u_Model", ve.Model);
                    heightCaptureShader->SetMat4("u_TopDownMVP", topDownVP * ve.Model);
                    RenderCommand::DrawIndexed(mr->Mesh);
                    gpuWaterHeightCaptured = true;
                    displacedWaterHeightCaptured = true;
                    ++gpuWaterCaptureDraws;
                }
                m_DeferredRenderer.EndHPWaterFluidWaterHeightCapture(gpuWaterHeightCaptured);
            }

            if (m_DeferredRenderer.BeginHPWaterFluidSceneHeightCapture(hpWaterFluidResolution,
                                                                       hpWaterFluidBoxCenter,
                                                                       hpWaterFluidBoxSize)) {
                heightCaptureShader->Bind();
                heightCaptureShader->SetVec3("u_BoxCenter", hpWaterFluidBoxCenter);
                heightCaptureShader->SetVec3("u_BoxSize", glm::max(hpWaterFluidBoxSize, glm::vec3(0.001f)));
                heightCaptureShader->SetFloat("u_ForceHeight", -1.0f);
                if (hpWaterFluidObstaclesEnabled) {
                    for (const auto& draw : hpWaterFluidSceneCaptureDraws) {
                        if (!draw.Mesh)
                            continue;
                        heightCaptureShader->SetMat4("u_Model", draw.Model);
                        heightCaptureShader->SetMat4("u_TopDownMVP", topDownVP * draw.Model);
                        RenderCommand::DrawIndexed(draw.Mesh);
                        sceneGeometryHeightCaptured = true;
                        ++gpuSceneCaptureDraws;
                    }
                }
                m_DeferredRenderer.EndHPWaterFluidSceneHeightCapture(true);
            }

            gpuHeightCaptureValid = m_DeferredRenderer.IsHPWaterFluidHeightCaptureValid();
            RenderCommand::SetViewport(0, 0, viewportWidth, viewportHeight);
        }

        const size_t fluidTexelCount =
            static_cast<size_t>(hpWaterFluidResolution) * static_cast<size_t>(hpWaterFluidResolution);
        std::vector<uint8_t> obstacleMask(fluidTexelCount, 0);
        std::vector<float> waterHeights(fluidTexelCount, 0.0f);
        std::vector<float> sceneHeights(fluidTexelCount, 0.0f);
        uint32_t obstacleCount = 0;
        uint32_t obstaclePixels = 0;

        std::fill(waterHeights.begin(), waterHeights.end(), normalizedWaterHeight);
        if (!hpWaterEntities.empty()) {
            struct WaterHeightSampleSource {
                HPWaterComponent* Water = nullptr;
                glm::mat4 Model {1.0f};
                glm::mat4 InverseModel {1.0f};
            };

            std::vector<WaterHeightSampleSource> waterSampleSources;
            waterSampleSources.reserve(hpWaterEntities.size());
            for (const auto& ve : hpWaterEntities) {
                auto* water = m_Registry.try_get<HPWaterComponent>(ve.ID);
                if (!water)
                    continue;

                waterSampleSources.push_back({ water, ve.Model, glm::inverse(ve.Model) });
            }

            const float halfX = hpWaterFluidBoxSize.x * 0.5f;
            const float halfZ = hpWaterFluidBoxSize.z * 0.5f;
            const float waterMinX = hpWaterFluidBoxCenter.x - halfX;
            const float waterMinZ = hpWaterFluidBoxCenter.z - halfZ;
            bool cpuDisplacedWaterHeights = false;
            for (uint32_t z = 0; z < hpWaterFluidResolution; ++z) {
                const float worldZ = waterMinZ +
                    (static_cast<float>(z) + 0.5f) / static_cast<float>(hpWaterFluidResolution) * hpWaterFluidBoxSize.z;
                for (uint32_t x = 0; x < hpWaterFluidResolution; ++x) {
                    const float worldX = waterMinX +
                        (static_cast<float>(x) + 0.5f) / static_cast<float>(hpWaterFluidResolution) * hpWaterFluidBoxSize.x;
                    float bestHeight = -std::numeric_limits<float>::max();
                    bool foundWater = false;
                    for (const auto& source : waterSampleSources) {
                        const glm::vec3 local = glm::vec3(source.InverseModel * glm::vec4(worldX, hpWaterFluidSurfaceY, worldZ, 1.0f));
                        if (std::abs(local.x) > source.Water->WorldSizeX * 0.5f ||
                            std::abs(local.z) > source.Water->WorldSizeZ * 0.5f) {
                            continue;
                        }

                        const float localHeight = SampleHPWaterLocalHeight(*source.Water, local.x, local.z);
                        const float worldHeight = glm::vec3(source.Model * glm::vec4(local.x, localHeight, local.z, 1.0f)).y;
                        bestHeight = std::max(bestHeight, normalizeHeight(worldHeight));
                        foundWater = true;
                    }

                    if (foundWater) {
                        waterHeights[static_cast<size_t>(z) * hpWaterFluidResolution + x] = bestHeight;
                        cpuDisplacedWaterHeights = true;
                    }
                }
            }
            displacedWaterHeightCaptured = displacedWaterHeightCaptured || cpuDisplacedWaterHeights;
        }

        if (hpWaterFluidObstaclesEnabled) {
            const float halfX = hpWaterFluidBoxSize.x * 0.5f;
            const float halfZ = hpWaterFluidBoxSize.z * 0.5f;
            const float waterMinX = hpWaterFluidBoxCenter.x - halfX;
            const float waterMinZ = hpWaterFluidBoxCenter.z - halfZ;
            const float waterMaxX = hpWaterFluidBoxCenter.x + halfX;
            const float waterMaxZ = hpWaterFluidBoxCenter.z + halfZ;
            const float invX = hpWaterFluidBoxSize.x > 0.0001f ? 1.0f / hpWaterFluidBoxSize.x : 0.0f;
            const float invZ = hpWaterFluidBoxSize.z > 0.0001f ? 1.0f / hpWaterFluidBoxSize.z : 0.0f;
            const float belowTolerance = 0.25f;

            for (const auto& obstacle : hpWaterFluidObstacleCandidates) {
                if (obstacle.Max.y < hpWaterFluidSurfaceY - belowTolerance ||
                    obstacle.Min.y > hpWaterFluidSurfaceY + hpWaterFluidObstacleHeightRange) {
                    continue;
                }

                const float paddedMinX = obstacle.Min.x - hpWaterFluidObstaclePadding;
                const float paddedMaxX = obstacle.Max.x + hpWaterFluidObstaclePadding;
                const float paddedMinZ = obstacle.Min.z - hpWaterFluidObstaclePadding;
                const float paddedMaxZ = obstacle.Max.z + hpWaterFluidObstaclePadding;

                if (paddedMaxX < waterMinX || paddedMinX > waterMaxX ||
                    paddedMaxZ < waterMinZ || paddedMinZ > waterMaxZ) {
                    continue;
                }

                int x0 = static_cast<int>(std::floor((paddedMinX - waterMinX) * invX * static_cast<float>(hpWaterFluidResolution)));
                int x1 = static_cast<int>(std::ceil ((paddedMaxX - waterMinX) * invX * static_cast<float>(hpWaterFluidResolution)));
                int z0 = static_cast<int>(std::floor((paddedMinZ - waterMinZ) * invZ * static_cast<float>(hpWaterFluidResolution)));
                int z1 = static_cast<int>(std::ceil ((paddedMaxZ - waterMinZ) * invZ * static_cast<float>(hpWaterFluidResolution)));

                x0 = std::clamp(x0, 0, static_cast<int>(hpWaterFluidResolution) - 1);
                x1 = std::clamp(x1, 0, static_cast<int>(hpWaterFluidResolution) - 1);
                z0 = std::clamp(z0, 0, static_cast<int>(hpWaterFluidResolution) - 1);
                z1 = std::clamp(z1, 0, static_cast<int>(hpWaterFluidResolution) - 1);
                if (x1 < x0 || z1 < z0)
                    continue;

                bool wroteAnyPixel = false;
                const float normalizedSceneHeight = normalizeHeight(obstacle.Max.y);
                for (int z = z0; z <= z1; ++z) {
                    for (int x = x0; x <= x1; ++x) {
                        const size_t idx = static_cast<size_t>(z) * static_cast<size_t>(hpWaterFluidResolution) +
                                           static_cast<size_t>(x);
                        sceneHeights[idx] = std::max(sceneHeights[idx], normalizedSceneHeight);
                        if (sceneHeights[idx] > waterHeights[idx] + 0.0005f && obstacleMask[idx] == 0) {
                            obstacleMask[idx] = 255;
                            ++obstaclePixels;
                            wroteAnyPixel = true;
                        }
                    }
                }

                if (wroteAnyPixel)
                    ++obstacleCount;
            }
        }

        m_RenderDiagnostics.HPWaterFluidObstacleCount = obstacleCount;
        m_RenderDiagnostics.HPWaterFluidObstaclePixels = obstaclePixels;
        m_RenderDiagnostics.HPWaterFluidHeightCaptureRan =
            m_DeferredRenderer.DidHPWaterFluidHeightCaptureRun();
        m_RenderDiagnostics.HPWaterFluidHeightCaptureValid = gpuHeightCaptureValid;
        m_RenderDiagnostics.HPWaterFluidCaptureSpaceParityEnabled = true;
        m_RenderDiagnostics.HPWaterFluidStartFrameBakeEnabled = hpWaterFluidStartFrameBake;
        m_RenderDiagnostics.HPWaterFluidHeightCaptureCacheReused =
            m_DeferredRenderer.WasHPWaterFluidHeightCaptureCacheReused();
        m_RenderDiagnostics.HPWaterFluidLayerFilteringParityEnabled = true;
        m_RenderDiagnostics.HPWaterFluidDisplacedWaterHeightCapture = displacedWaterHeightCaptured;
        m_RenderDiagnostics.HPWaterFluidSceneGeometryHeightCapture = sceneGeometryHeightCaptured;
        m_RenderDiagnostics.HPWaterFluidWaterCaptureDraws = gpuWaterCaptureDraws;
        m_RenderDiagnostics.HPWaterFluidSceneCaptureDraws = gpuSceneCaptureDraws;
        if (!gpuHeightCaptureValid) {
            m_RenderDiagnostics.HPWaterFluidHeightFieldValid =
                m_DeferredRenderer.UploadHPWaterFluidHeightFields(hpWaterFluidResolution,
                                                                  waterHeights,
                                                                  sceneHeights,
                                                                  hpWaterFluidBoxCenter,
                                                                  hpWaterFluidBoxSize);
        } else {
            m_RenderDiagnostics.HPWaterFluidHeightFieldValid =
                m_DeferredRenderer.IsHPWaterFluidHeightFieldValid();
        }
        m_RenderDiagnostics.HPWaterFluidObstacleValid =
            m_DeferredRenderer.UploadHPWaterFluidObstacleMask(hpWaterFluidResolution,
                                                              obstacleMask,
                                                              hpWaterFluidBoxCenter,
                                                              hpWaterFluidBoxSize);
        m_RenderDiagnostics.HPWaterFluidDynamicsRan =
            m_DeferredRenderer.UpdateHPWaterFluidDynamics(hpWaterFluidResolution,
                                                          hpWaterFluidWaveSpeed,
                                                          hpWaterFluidDamping,
                                                          hpWaterFluidSourceU,
                                                          hpWaterFluidSourceV,
                                                          hpWaterFluidSourceIntensity,
                                                          hpWaterFluidSourceRadius,
                                                          hpWaterFluidBoxCenter,
                                                          hpWaterFluidBoxSize);
    }

    if (!hpWaterEntities.empty()) {
        auto hpWaterShader = m_DeferredRenderer.GetHPWaterGBufferShader();
        if (hpWaterShader) {
            m_DeferredRenderer.BeginHPWaterGBufferPass();
            hpWaterShader->Bind();

            for (const auto& ve : hpWaterEntities) {
                auto* water = m_Registry.try_get<HPWaterComponent>(ve.ID);
                auto* mr = m_Registry.try_get<MeshRendererComponent>(ve.ID);
                if (!water || !water->Enabled || !mr || !mr->Mesh)
                    continue;

                std::shared_ptr<VertexArray> drawVAO = mr->Mesh;
                auto* ac = m_Registry.try_get<AnimatorComponent>(ve.ID);
                if (ac && ac->_Animator && ac->_Animator->GetSkinnedVAO())
                    drawVAO = ac->_Animator->GetSkinnedVAO();

                hpWaterShader->SetMat4("u_MVP", viewProjection * ve.Model);
                hpWaterShader->SetMat4("u_Model", ve.Model);
                hpWaterShader->SetVec3("u_HPScatterColor", glm::vec3(
                    water->ScatterColor[0],
                    water->ScatterColor[1],
                    water->ScatterColor[2]));
                hpWaterShader->SetVec3("u_HPAbsorptionColor", glm::vec3(
                    water->AbsorptionColor[0],
                    water->AbsorptionColor[1],
                    water->AbsorptionColor[2]));
                hpWaterShader->SetVec3("u_HPFoamColor", glm::vec3(
                    water->FoamColor[0],
                    water->FoamColor[1],
                    water->FoamColor[2]));
                hpWaterShader->SetFloat("u_HPFoamIntensity", water->FoamIntensity);
                hpWaterShader->SetFloat("u_HPRoughness", water->Roughness);
                hpWaterShader->SetFloat("u_HPThickness", water->DepthTintDistance);
                hpWaterShader->SetFloat("u_HPHeightScale", water->HeightScale);
                hpWaterShader->SetFloat("u_HPBaseHeight", water->BaseHeight);
                hpWaterShader->SetInt("u_HPSpectrumWaves", water->SpectrumWaves ? 1 : 0);
                hpWaterShader->SetFloat("u_HPSpectrumAmplitude", water->SpectrumAmplitude);
                hpWaterShader->SetFloat("u_HPSpectrumWindAngle", water->SpectrumWindAngle);
                hpWaterShader->SetFloat("u_HPSpectrumTime", water->_OceanTime);
                hpWaterShader->SetFloat("u_HPSpectrumNormalStrength", water->SpectrumNormalStrength);
                hpWaterShader->SetFloat("u_HPChoppiness", water->Choppiness);
                const bool fluidValid = m_DeferredRenderer.IsHPWaterFluidDynamicsValid() && water->FluidDynamicsEnabled;
                glActiveTexture(GL_TEXTURE8);
                glBindTexture(GL_TEXTURE_2D, fluidValid
                    ? static_cast<GLuint>(m_DeferredRenderer.GetHPWaterFluidHeightTexture())
                    : 0);
                hpWaterShader->SetInt("u_HPFluidHeightTexture", 8);
                hpWaterShader->SetInt("u_HPFluidDynamicsEnabled", fluidValid ? 1 : 0);
                hpWaterShader->SetVec3("u_HPFluidBoxCenter", m_DeferredRenderer.GetHPWaterFluidBoxCenter());
                hpWaterShader->SetVec3("u_HPFluidBoxSize", m_DeferredRenderer.GetHPWaterFluidBoxSize());
                hpWaterShader->SetFloat("u_HPFluidHeightScale", water->HeightScale);

                m_RenderDiagnostics.HPWaterSpectralOceanEnabled =
                    m_RenderDiagnostics.HPWaterSpectralOceanEnabled ||
                    (water->SpectrumWaves && water->SpectrumAmplitude > 0.0f);
                m_RenderDiagnostics.HPWaterSpectralNormalParityEnabled =
                    m_RenderDiagnostics.HPWaterSpectralNormalParityEnabled ||
                    (water->SpectrumWaves && water->SpectrumAmplitude > 0.0f &&
                     water->SpectrumNormalStrength > 0.0f);
                m_RenderDiagnostics.HPWaterSpectrumAmplitude =
                    std::max(m_RenderDiagnostics.HPWaterSpectrumAmplitude, water->SpectrumAmplitude);
                m_RenderDiagnostics.HPWaterSpectrumNormalStrength =
                    std::max(m_RenderDiagnostics.HPWaterSpectrumNormalStrength, water->SpectrumNormalStrength);

                RenderCommand::DrawIndexed(drawVAO);
                m_RenderDiagnostics.HPWaterGBufferDrawn++;
            }

            m_DeferredRenderer.EndHPWaterGBufferPass();
            if (m_RenderDiagnostics.HPWaterGBufferDrawn > 0) {
                m_RenderDiagnostics.HPWaterDepthPyramidRan =
                    m_DeferredRenderer.BuildHPWaterDepthPyramid();
                m_RenderDiagnostics.HPWaterDepthMergedToSceneDepth =
                    m_DeferredRenderer.MergeHPWaterDepthIntoSceneDepth();
                m_RenderDiagnostics.HPWaterNormalMergedToSceneGBuffer =
                    m_RenderDiagnostics.HPWaterDepthMergedToSceneDepth &&
                    m_DeferredRenderer.MergeHPWaterNormalIntoSceneGBuffer();
                m_RenderDiagnostics.HPWaterStencilMarkedInSceneDepth =
                    m_DeferredRenderer.IsHPWaterStencilMarkedInSceneDepth();
                m_RenderDiagnostics.HPWaterStencilRef = m_DeferredRenderer.GetHPWaterStencilRef();
            }

            if (!hpWaterEntities.empty() && hpWaterCausticAtlasEnabled &&
                ps.ShadowsEnabled && m_ShadowMap.IsInitialized()) {
                auto atlasShader = m_DeferredRenderer.GetHPWaterCausticAtlasShader();
                if (atlasShader && m_DeferredRenderer.BeginHPWaterCausticAtlas(hpWaterCausticAtlasResolution)) {
                    atlasShader->Bind();

                    for (uint32_t cascade = 0; cascade < static_cast<uint32_t>(ShadowMap::NUM_CASCADES); ++cascade) {
                        m_DeferredRenderer.BeginHPWaterCausticAtlasCascade(cascade);
                        const glm::mat4 lightVP = m_ShadowMap.GetLightViewProjection(static_cast<int>(cascade));

                        for (const auto& ve : hpWaterEntities) {
                            auto* water = m_Registry.try_get<HPWaterComponent>(ve.ID);
                            auto* mr = m_Registry.try_get<MeshRendererComponent>(ve.ID);
                            if (!water || !water->Enabled || !mr || !mr->Mesh)
                                continue;

                            std::shared_ptr<VertexArray> drawVAO = mr->Mesh;
                            auto* ac = m_Registry.try_get<AnimatorComponent>(ve.ID);
                            if (ac && ac->_Animator && ac->_Animator->GetSkinnedVAO())
                                drawVAO = ac->_Animator->GetSkinnedVAO();

                            atlasShader->SetMat4("u_LightMVP", lightVP * ve.Model);
                            atlasShader->SetMat4("u_Model", ve.Model);
                            atlasShader->SetFloat("u_HPRoughness", water->Roughness);
                            atlasShader->SetFloat("u_HPThickness", water->DepthTintDistance);
                            RenderCommand::DrawIndexed(drawVAO);
                            ++hpWaterCausticAtlasDrawn;
                        }
                    }

                    m_DeferredRenderer.EndHPWaterCausticAtlas(hpWaterCausticAtlasDrawn > 0);
                    RenderCommand::SetViewport(0, 0, viewportWidth, viewportHeight);
                }
            }
            m_RenderDiagnostics.HPWaterCausticAtlasRan = hpWaterCausticAtlasDrawn > 0;
            m_RenderDiagnostics.HPWaterCausticAtlasDrawn = hpWaterCausticAtlasDrawn;
        }
    }

    // ── Phase 2: Deferred Lighting Pass ──
    {
        auto lightingShader = m_DeferredRenderer.GetLightingShader();
        if (lightingShader) {
            lightingShader->Bind();
            setLightingUniforms(lightingShader);
            // Bind shadow map
            if (ps.ShadowsEnabled && m_ShadowMap.IsInitialized()) {
                m_ShadowMap.BindToShader(lightingShader, shadowSettings);
                lightingShader->SetMat4("u_ShadowCameraView", cameraView);
            } else {
                lightingShader->SetInt("u_ShadowsEnabled", 0);
            }
        }
        m_DeferredRenderer.LightingPass();
        m_RenderDiagnostics.LightingPassRan = true;
        m_RenderDiagnostics.DeferredOutputTexture = m_DeferredRenderer.GetOutputTexture();
    }

    // ── Phase 3: Forward pass for transparent objects ──
    // Transparent objects cannot be deferred-rendered; they need the forward path.
    if (!transparentEntities.empty()) {
        m_RenderDiagnostics.ForwardPassRan = true;
        m_DeferredRenderer.BeginForwardPass();

        // Sort back-to-front
        std::sort(transparentEntities.begin(), transparentEntities.end(),
            [](const VisibleEntity& a, const VisibleEntity& b) {
                return a.DistSq > b.DistSq;
            });

        // Helper to set lighting on forward shader for transparent
        auto setForwardLighting = [&](const std::shared_ptr<Shader>& shader, bool isLit) {
            if (!isLit) return;
            shader->SetVec3("u_LightDir", lightDir);
            shader->SetVec3("u_LightColor", lightColor);
            shader->SetFloat("u_LightIntensity", lightIntensity);
            shader->SetVec3("u_ViewPos", cameraPos);
            shader->SetInt("u_IndirectLightingEnabled", ps.IndirectLightingEnabled ? 1 : 0);
            shader->SetVec3("u_IndirectSkyColor", indirectSkyColor);
            shader->SetVec3("u_IndirectGroundColor", indirectGroundColor);
            shader->SetVec3("u_IndirectTint", indirectTint);
            shader->SetFloat("u_IndirectDiffuseIntensity", ps.IndirectDiffuseIntensity);
            shader->SetFloat("u_SkyReflectionIntensity", ps.SkyReflectionIntensity);
            BindDummyReflectionProbe(shader);
            shader->SetInt("u_NumPointLights", numPointLights);
            for (int i = 0; i < numPointLights; ++i) {
                shader->SetVec3(s_PointLightPositions[i], pointPositions[i]);
                shader->SetVec3(s_PointLightColors[i], pointColors[i]);
                shader->SetFloat(s_PointLightIntensities[i], pointIntensities[i]);
                shader->SetFloat(s_PointLightRanges[i], pointRanges[i]);
            }
            shader->SetInt("u_NumSpotLights", numSpotLights);
            for (int i = 0; i < numSpotLights; ++i) {
                shader->SetVec3(s_SpotLightPositions[i], spotPositions[i]);
                shader->SetVec3(s_SpotLightDirections[i], spotDirections[i]);
                shader->SetVec3(s_SpotLightColors[i], spotColors[i]);
                shader->SetFloat(s_SpotLightIntensities[i], spotIntensities[i]);
                shader->SetFloat(s_SpotLightRanges[i], spotRanges[i]);
                shader->SetFloat(s_SpotLightInnerCos[i], spotInnerCos[i]);
                shader->SetFloat(s_SpotLightOuterCos[i], spotOuterCos[i]);
            }
            // Bind shadow map for forward transparent pass
            if (ps.ShadowsEnabled && m_ShadowMap.IsInitialized()) {
                m_ShadowMap.BindToShader(shader, shadowSettings);
                shader->SetMat4("u_ShadowCameraView", cameraView);
            } else {
                shader->SetInt("u_ShadowsEnabled", 0);
            }
        };

        for (auto& ve : transparentEntities) {
            auto* mr = m_Registry.try_get<MeshRendererComponent>(ve.ID);
            if (!mr || !mr->Mesh || !mr->Mat) continue;

            mr->Mat->Bind();
            auto shader = mr->Mat->GetShader();
            if (!shader) continue;

            glm::mat4 mvp = viewProjection * ve.Model;
            glm::vec4 entityColor(mr->Color[0], mr->Color[1], mr->Color[2], mr->Color[3]);

            shader->SetMat4("u_MVP", mvp);
            shader->SetMat4("u_Model", ve.Model);
            shader->SetVec4("u_EntityColor", entityColor);

            if (auto* water = m_Registry.try_get<HPWaterComponent>(ve.ID)) {
                shader->SetInt("u_HPWaterEnabled", water->Enabled ? 1 : 0);
                shader->SetVec4("u_WaterColor", glm::vec4(
                    water->ScatterColor[0], water->ScatterColor[1], water->ScatterColor[2], 0.86f));
                shader->SetVec4("u_DeepColor", glm::vec4(
                    water->AbsorptionColor[0], water->AbsorptionColor[1], water->AbsorptionColor[2], 1.0f));
                shader->SetVec3("u_HPScatterColor", glm::vec3(
                    water->ScatterColor[0], water->ScatterColor[1], water->ScatterColor[2]));
                shader->SetVec3("u_HPAbsorptionColor", glm::vec3(
                    water->AbsorptionColor[0], water->AbsorptionColor[1], water->AbsorptionColor[2]));
                shader->SetVec3("u_HPFoamColor", glm::vec3(
                    water->FoamColor[0], water->FoamColor[1], water->FoamColor[2]));
                shader->SetFloat("u_HPFoamIntensity", water->FoamIntensity);
                shader->SetFloat("u_HPRoughness", water->Roughness);
                shader->SetFloat("u_HPRefractionStrength", water->RefractionStrength);
                shader->SetFloat("u_HPDepthTintDistance", water->DepthTintDistance);
                shader->SetFloat("u_HPHeightScale", water->HeightScale);
            } else {
                shader->SetInt("u_HPWaterEnabled", 0);
            }

            if (mr->Mat->IsLit())
                setForwardLighting(shader, true);

            RenderCommand::DrawIndexed(mr->Mesh);
            m_RenderDiagnostics.TransparentDrawn++;
            if (m_Registry.any_of<HPWaterComponent>(ve.ID))
                m_RenderDiagnostics.HPWaterDrawn++;
        }
        m_DeferredRenderer.EndForwardPass();
    }

    if (m_RenderDiagnostics.HPWaterGBufferDrawn > 0) {
        glm::mat4 inverseViewProjection = glm::inverse(viewProjection);
        m_RenderDiagnostics.HPWaterRefractionStrength = hpWaterRefractionStrength;
        m_RenderDiagnostics.HPWaterMaxRefractionCrossDistance = hpWaterMaxRefractionCrossDistance;
        m_RenderDiagnostics.HPWaterRefractionThicknessOffset = hpWaterRefractionThicknessOffset;
        m_RenderDiagnostics.HPWaterRefractionSampleCount = hpWaterRefractionSampleCount;
        m_RenderDiagnostics.HPWaterRefractionJitterEnabled = hpWaterRefractionJitter;
        m_RenderDiagnostics.HPWaterEnvironmentReflectionIntensity = hpWaterEnvironmentReflectionIntensity;
        m_RenderDiagnostics.HPWaterIndirectLightStrength = hpWaterIndirectLightStrength;
        m_RenderDiagnostics.HPWaterMacroScatterStrength = hpWaterMacroScatterStrength;
        m_RenderDiagnostics.HPWaterThinSSSStrength = hpWaterThinSSSStrength;
        m_RenderDiagnostics.HPWaterBacklitTransmissionStrength = hpWaterBacklitTransmissionStrength;
        m_RenderDiagnostics.HPWaterForwardScatterStrength = hpWaterForwardScatterStrength;
        m_RenderDiagnostics.HPWaterForwardScatterBlurDensity = hpWaterForwardScatterBlurDensity;
        m_RenderDiagnostics.HPWaterMultiScatterScale = hpWaterMultiScatterScale;
        m_RenderDiagnostics.HPWaterSpecularFGDStrength = hpWaterSpecularFGDStrength;
        m_RenderDiagnostics.HPWaterGGXEnergyCompensation = hpWaterGGXEnergyCompensation;
        m_RenderDiagnostics.HPWaterLightLoopInputsValid = true;
        m_RenderDiagnostics.HPWaterIndirectScatterIntegrationEnabled =
            ps.IndirectLightingEnabled && hpWaterIndirectLightStrength > 0.0001f;
        m_RenderDiagnostics.HPWaterBSDFComponentWeightingEnabled =
            hpWaterForwardScatterStrength > 0.0001f ||
            hpWaterThinSSSStrength > 0.0001f ||
            hpWaterBacklitTransmissionStrength > 0.0001f;
        m_RenderDiagnostics.HPWaterSkyReflectionIntensity = ps.SkyReflectionIntensity;
        m_RenderDiagnostics.HPWaterIndirectDiffuseIntensity =
            ps.IndirectLightingEnabled ? ps.IndirectDiffuseIntensity * hpWaterIndirectLightStrength : 0.0f;
        m_RenderDiagnostics.HPWaterDirectionalLightIntensity = lightIntensity;
        m_RenderDiagnostics.HPWaterPointLightCount = static_cast<uint32_t>(numPointLights);
        m_RenderDiagnostics.HPWaterSpotLightCount = static_cast<uint32_t>(numSpotLights);
        m_RenderDiagnostics.HPWaterPunctualLightLoopEnabled = (numPointLights + numSpotLights) > 0;
        const uint32_t hpWaterSkyTexture =
            ps.SkyTexture ? static_cast<uint32_t>(ps.SkyTexture->GetNativeTextureID()) : 0u;
        uint32_t hpWaterReflectionProbeTexture = 0;
        uint32_t hpWaterReflectionProbeSecondaryTexture = 0;
        glm::vec3 hpWaterReflectionProbeCenter(0.0f);
        glm::vec3 hpWaterReflectionProbeBoxSize(1.0f);
        glm::vec3 hpWaterReflectionProbeSecondaryCenter(0.0f);
        glm::vec3 hpWaterReflectionProbeSecondaryBoxSize(1.0f);
        float hpWaterReflectionProbeIntensity = 0.0f;
        float hpWaterReflectionProbeBlend = 0.0f;
        float hpWaterReflectionProbeInfluenceWeight = 0.0f;
        float hpWaterReflectionProbeHierarchyWeight = 0.0f;
        {
            struct ProbeCandidate {
                uint32_t Texture = 0;
                glm::vec3 Center = glm::vec3(0.0f);
                glm::vec3 BoxSize = glm::vec3(1.0f);
                float Influence = 0.0f;
                float SelectionWeight = 0.0f;
            };

            auto computeBoxInfluence = [](const glm::vec3& position,
                                          const glm::vec3& center,
                                          const glm::vec3& boxSize) -> float {
                constexpr float fadeStart = 0.75f;
                const glm::vec3 halfSize = glm::max(boxSize * 0.5f, glm::vec3(0.001f));
                const glm::vec3 normalizedDistance = glm::abs(position - center) / halfSize;
                const float edgeFactor = glm::max(glm::max(normalizedDistance.x, normalizedDistance.y),
                                                  normalizedDistance.z);
                if (edgeFactor >= 1.0f)
                    return 0.0f;
                if (edgeFactor <= fadeStart)
                    return 1.0f;
                return glm::clamp((1.0f - edgeFactor) / (1.0f - fadeStart), 0.0f, 1.0f);
            };

            ProbeCandidate primaryProbe;
            ProbeCandidate secondaryProbe;
            auto probeView = m_Registry.view<TransformComponent, ReflectionProbeComponent>();
            for (auto probeEntity : probeView) {
                if (!IsEntityActiveInHierarchy(probeEntity))
                    continue;

                auto& probe = probeView.get<ReflectionProbeComponent>(probeEntity);
                if (!probe._Probe || !probe._Probe->IsBaked() || probe._Probe->GetCubemapID() == 0)
                    continue;

                glm::vec3 probePos = glm::vec3(GetWorldTransform(probeEntity)[3]);
                glm::vec3 probeBoxSize = glm::max(
                    glm::vec3(probe.BoxSize[0], probe.BoxSize[1], probe.BoxSize[2]),
                    glm::vec3(0.001f));
                const float influence = computeBoxInfluence(cameraPos, probePos, probeBoxSize);
                if (influence <= 0.0f)
                    continue;

                const glm::vec3 toCamera = probePos - cameraPos;
                const float distance = std::sqrt(std::max(glm::dot(toCamera, toCamera), 0.0f));
                const float selectionWeight = influence / std::max(distance, 0.25f);
                ProbeCandidate candidate{
                    probe._Probe->GetCubemapID(),
                    probePos,
                    probeBoxSize,
                    influence,
                    selectionWeight
                };

                if (candidate.SelectionWeight > primaryProbe.SelectionWeight) {
                    secondaryProbe = primaryProbe;
                    primaryProbe = candidate;
                } else if (candidate.SelectionWeight > secondaryProbe.SelectionWeight) {
                    secondaryProbe = candidate;
                }
            }

            if (primaryProbe.Texture != 0) {
                hpWaterReflectionProbeTexture = primaryProbe.Texture;
                hpWaterReflectionProbeCenter = primaryProbe.Center;
                hpWaterReflectionProbeBoxSize = primaryProbe.BoxSize;
                hpWaterReflectionProbeInfluenceWeight = primaryProbe.Influence;
                hpWaterReflectionProbeHierarchyWeight = primaryProbe.Influence;
                hpWaterReflectionProbeIntensity = primaryProbe.Influence;

                if (secondaryProbe.Texture != 0) {
                    hpWaterReflectionProbeSecondaryTexture = secondaryProbe.Texture;
                    hpWaterReflectionProbeSecondaryCenter = secondaryProbe.Center;
                    hpWaterReflectionProbeSecondaryBoxSize = secondaryProbe.BoxSize;
                    const float totalSelectionWeight =
                        std::max(primaryProbe.SelectionWeight + secondaryProbe.SelectionWeight, 0.0001f);
                    hpWaterReflectionProbeBlend = secondaryProbe.SelectionWeight / totalSelectionWeight;
                    hpWaterReflectionProbeHierarchyWeight =
                        glm::clamp(primaryProbe.Influence +
                                       secondaryProbe.Influence * (1.0f - primaryProbe.Influence),
                                   0.0f,
                                   1.0f);
                    hpWaterReflectionProbeIntensity = hpWaterReflectionProbeHierarchyWeight;
                }
            }
        }
        const bool hpWaterReflectionProbeBound = hpWaterReflectionProbeTexture != 0;
        m_RenderDiagnostics.HPWaterSkyTextureReflectionBound = hpWaterSkyTexture != 0;
        m_RenderDiagnostics.HPWaterSkyTexture = hpWaterSkyTexture;
        m_RenderDiagnostics.HPWaterReflectionProbeBound = hpWaterReflectionProbeBound;
        m_RenderDiagnostics.HPWaterReflectionProbeTexture = hpWaterReflectionProbeTexture;
        m_RenderDiagnostics.HPWaterReflectionProbeSecondaryTexture = hpWaterReflectionProbeSecondaryTexture;
        m_RenderDiagnostics.HPWaterReflectionProbeIntensity = hpWaterReflectionProbeIntensity;
        m_RenderDiagnostics.HPWaterReflectionProbeBlend = hpWaterReflectionProbeBlend;
        m_RenderDiagnostics.HPWaterReflectionProbeInfluenceWeight = hpWaterReflectionProbeInfluenceWeight;
        m_RenderDiagnostics.HPWaterReflectionProbeHierarchyWeight = hpWaterReflectionProbeHierarchyWeight;
        m_RenderDiagnostics.HPWaterReflectionProbeInfluenceBlendEnabled =
            hpWaterReflectionProbeBound && hpWaterReflectionProbeHierarchyWeight > 0.0f;
        m_RenderDiagnostics.HPWaterReflectionProbeBoxProjectionEnabled = hpWaterReflectionProbeBound;
        m_RenderDiagnostics.HPWaterSSRReflectionEnabled = ps.SSREnabled;
        m_RenderDiagnostics.HPWaterSSRHierarchyBlendEnabled = ps.SSREnabled;
        m_RenderDiagnostics.HPWaterSSRMaxSteps = static_cast<uint32_t>(std::max(ps.SSRMaxSteps, 0));
        m_RenderDiagnostics.HPWaterSSRStepSize = ps.SSRStepSize;
        m_RenderDiagnostics.HPWaterSSRThickness = ps.SSRThickness;
        m_RenderDiagnostics.HPWaterSSRMaxDistance = ps.SSRMaxDistance;
        m_RenderDiagnostics.HPWaterReflectionProbeCenter = hpWaterReflectionProbeCenter;
        m_RenderDiagnostics.HPWaterReflectionProbeBoxSize = hpWaterReflectionProbeBoxSize;
        m_RenderDiagnostics.HPWaterReflectionProbeSecondaryCenter =
            hpWaterReflectionProbeSecondaryTexture != 0 ? hpWaterReflectionProbeSecondaryCenter : hpWaterReflectionProbeCenter;
        m_RenderDiagnostics.HPWaterReflectionProbeSecondaryBoxSize =
            hpWaterReflectionProbeSecondaryTexture != 0 ? hpWaterReflectionProbeSecondaryBoxSize : hpWaterReflectionProbeBoxSize;
        m_RenderDiagnostics.HPWaterCausticStrength = hpWaterCausticStrength;
        m_RenderDiagnostics.HPWaterCausticScale = hpWaterCausticScale;
        m_RenderDiagnostics.HPWaterCausticDepthFade = hpWaterCausticDepthFade;
        m_RenderDiagnostics.HPWaterCausticTransmittanceMaskEnabled =
            hpWaterCausticTransmittanceStrength > 0.0001f || hpWaterCausticLeakReduction > 0.0001f;
        m_RenderDiagnostics.HPWaterCausticTransmittanceStrength = hpWaterCausticTransmittanceStrength;
        m_RenderDiagnostics.HPWaterCausticLeakReduction = hpWaterCausticLeakReduction;
        m_RenderDiagnostics.HPWaterCausticShadowAlphaClipThreshold = hpWaterCausticShadowAlphaClipThreshold;
        m_RenderDiagnostics.HPWaterCausticScatterBoost = hpWaterCausticScatterBoost;
        m_RenderDiagnostics.HPWaterCausticRGBDispersion = hpWaterCausticRGBDispersion;
        m_RenderDiagnostics.HPWaterCausticDispersionStrength = hpWaterCausticDispersionStrength;
        m_RenderDiagnostics.HPWaterWaterDispersionStrength = hpWaterWaterDispersionStrength;
        m_RenderDiagnostics.HPWaterPhaseG = hpWaterPhaseG;
        m_RenderDiagnostics.HPWaterCausticAtlasTileResolution = hpWaterCausticAtlasResolution;
        m_RenderDiagnostics.HPWaterCausticAtlasCascades = m_DeferredRenderer.GetHPWaterCausticAtlasCascadeCount();
        m_RenderDiagnostics.HPWaterCausticAtlasDrawn = hpWaterCausticAtlasDrawn;
        m_RenderDiagnostics.HPWaterCausticFilterRadius = hpWaterCausticFilterRadius;
        m_RenderDiagnostics.HPWaterCausticFilterDepthSigma = hpWaterCausticFilterDepthSigma;
        m_RenderDiagnostics.HPWaterCausticFilterLuminanceWeight = hpWaterCausticFilterLuminanceWeight;
        m_RenderDiagnostics.HPWaterCausticVolumeStrength = hpWaterCausticVolumeStrength;
        const uint32_t hpWaterFrameIndex = static_cast<uint32_t>(m_RenderDiagnostics.FrameIndex & 0xffffffffULL);
        if (!m_RenderDiagnostics.HPWaterDepthPyramidRan &&
            !m_RenderDiagnostics.HPWaterDepthMergedToSceneDepth) {
            m_RenderDiagnostics.HPWaterDepthPyramidRan = m_DeferredRenderer.BuildHPWaterDepthPyramid();
        }
        m_RenderDiagnostics.HPWaterMaskRan = m_DeferredRenderer.BuildHPWaterMask();
        std::array<glm::mat4, 4> hpWaterCascadeVP{};
        std::array<float, 4> hpWaterCascadeSplits{};
        if (m_ShadowMap.IsInitialized()) {
            const auto& splits = m_ShadowMap.GetCascadeSplitDistances();
            for (int i = 0; i < ShadowMap::NUM_CASCADES; ++i) {
                hpWaterCascadeVP[static_cast<size_t>(i)] = m_ShadowMap.GetLightViewProjection(i);
                hpWaterCascadeSplits[static_cast<size_t>(i)] = splits[static_cast<size_t>(i)];
            }
        }
        const uint32_t hpWaterShadowDepthTextureArray =
            m_ShadowMap.IsInitialized() ? m_ShadowMap.GetTextureArrayID() : 0u;
        const uint32_t hpWaterShadowDepthResolution =
            m_ShadowMap.IsInitialized() ? static_cast<uint32_t>(m_ShadowMap.GetResolution()) : 0u;
        // First composite pass generates the full-resolution refraction payload
        // consumed by the low-resolution HPWater volume pass.
        m_DeferredRenderer.CompositeHPWater(nearClip,
                                            farClip,
                                            hpWaterRefractionStrength,
                                            hpWaterWaterDispersionStrength,
                                            hpWaterMaxRefractionCrossDistance,
                                            hpWaterRefractionThicknessOffset,
                                            static_cast<int>(hpWaterRefractionSampleCount),
                                            hpWaterRefractionJitter,
                                            hpWaterFrameIndex,
                                            hpWaterEnvironmentReflectionIntensity,
                                            hpWaterIndirectLightStrength,
                                            hpWaterThinSSSStrength,
                                            hpWaterBacklitTransmissionStrength,
                                            hpWaterForwardScatterStrength,
                                            hpWaterForwardScatterBlurDensity,
                                            hpWaterMultiScatterScale,
                                            hpWaterPhaseG,
                                            hpWaterSpecularFGDStrength,
                                            hpWaterGGXEnergyCompensation,
                                            cameraPos,
                                            lightDir,
                                            lightColor,
                                            lightIntensity,
                                            numPointLights,
                                            pointPositions,
                                            pointColors,
                                            pointIntensities,
                                            pointRanges,
                                            numSpotLights,
                                            spotPositions,
                                            spotDirections,
                                            spotColors,
                                            spotIntensities,
                                            spotRanges,
                                            spotInnerCos,
                                            spotOuterCos,
                                            cameraView,
                                            hpWaterCascadeVP,
                                            hpWaterCascadeSplits,
                                            hpWaterShadowDepthTextureArray,
                                            hpWaterShadowDepthResolution,
                                            ps.ShadowsEnabled && m_ShadowMap.IsInitialized(),
                                            shadowSettings.DepthBias,
                                            shadowSettings.NormalBias,
                                            shadowSettings.PCFQuality,
                                            shadowSettings.CascadeBlendWidth,
                                            indirectSkyColor,
                                            indirectGroundColor,
                                            indirectTint,
                                            ps.IndirectLightingEnabled,
                                            ps.IndirectDiffuseIntensity,
                                            ps.SkyReflectionIntensity,
                                            ps.SSREnabled,
                                            ps.SSRMaxSteps,
                                            ps.SSRStepSize,
                                            ps.SSRThickness,
                                            ps.SSRMaxDistance,
                                            hpWaterSkyTexture,
                                            hpWaterReflectionProbeTexture,
                                            hpWaterReflectionProbeSecondaryTexture,
                                            hpWaterReflectionProbeBound,
                                            hpWaterReflectionProbeIntensity,
                                            hpWaterReflectionProbeBlend,
                                            hpWaterReflectionProbeHierarchyWeight,
                                            hpWaterReflectionProbeCenter,
                                            hpWaterReflectionProbeBoxSize,
                                            hpWaterReflectionProbeSecondaryTexture != 0
                                                ? hpWaterReflectionProbeSecondaryCenter
                                                : hpWaterReflectionProbeCenter,
                                            hpWaterReflectionProbeSecondaryTexture != 0
                                                ? hpWaterReflectionProbeSecondaryBoxSize
                                                : hpWaterReflectionProbeBoxSize,
                                            viewProjection,
                                            inverseViewProjection);
        m_RenderDiagnostics.HPWaterCausticRan =
            hpWaterCausticsEnabled &&
            m_DeferredRenderer.AccumulateHPWaterCaustics(nearClip,
                                                         farClip,
                                                         lightDir,
                                                         lightColor,
                                                         lightIntensity,
                                                         viewProjection,
                                                         inverseViewProjection,
                                                         hpWaterCascadeVP,
                                                         hpWaterCascadeSplits,
                                                         hpWaterShadowDepthTextureArray,
                                                         hpWaterShadowDepthResolution,
                                                         hpWaterFrameIndex,
                                                         hpWaterCausticStrength,
                                                         hpWaterCausticScale,
                                                         hpWaterCausticDepthFade,
                                                         hpWaterCausticTransmittanceStrength,
                                                         hpWaterCausticLeakReduction,
                                                         hpWaterCausticShadowAlphaClipThreshold,
                                                         hpWaterCausticScatterBoost,
                                                         hpWaterCausticRGBDispersion,
                                                         hpWaterCausticDispersionStrength);
        m_RenderDiagnostics.HPWaterCausticFilterRan =
            m_RenderDiagnostics.HPWaterCausticRan &&
            hpWaterCausticFilterEnabled &&
            m_DeferredRenderer.FilterHPWaterCaustics(hpWaterCausticFilterRadius,
                                                     hpWaterCausticFilterDepthSigma,
                                                     hpWaterCausticFilterLuminanceWeight,
                                                     hpWaterCausticFilterIterations);
        m_RenderDiagnostics.HPWaterCausticFilterKernelParityEnabled =
            m_RenderDiagnostics.HPWaterCausticFilterRan;
        m_RenderDiagnostics.HPWaterVolumeRan =
            m_DeferredRenderer.AccumulateHPWaterVolume(nearClip,
                                                       farClip,
                                                       lightDir,
                                                       lightColor,
                                                       lightIntensity,
                                                       numPointLights,
                                                       pointPositions,
                                                       pointColors,
                                                       pointIntensities,
                                                       pointRanges,
                                                       numSpotLights,
                                                       spotPositions,
                                                       spotDirections,
                                                       spotColors,
                                                       spotIntensities,
                                                       spotRanges,
                                                       spotInnerCos,
                                                       spotOuterCos,
                                                       cameraPos,
                                                       inverseViewProjection,
                                                       cameraView,
                                                       hpWaterCascadeVP,
                                                       hpWaterCascadeSplits,
                                                       hpWaterShadowDepthTextureArray,
                                                       hpWaterShadowDepthResolution,
                                                       ps.ShadowsEnabled && m_ShadowMap.IsInitialized(),
                                                       shadowSettings.DepthBias,
                                                       shadowSettings.NormalBias,
                                                       shadowSettings.PCFQuality,
                                                       shadowSettings.CascadeBlendWidth,
                                                       hpWaterMacroScatterStrength,
                                                       hpWaterVolumeShadowSoftness,
                                                       hpWaterVolumeShadowMinFilterSize,
                                                       hpWaterVolumeShadowBlockerSamples,
                                                       hpWaterVolumeShadowFilterSamples,
                                                       hpWaterCausticVolumeStrength,
                                                       hpWaterFrameIndex);
        m_RenderDiagnostics.HPWaterVolumeExponentialIntegrationEnabled =
            m_DeferredRenderer.IsHPWaterVolumeExponentialIntegrationEnabled();
        m_RenderDiagnostics.HPWaterVolumeShadowSamplingEnabled =
            m_DeferredRenderer.IsHPWaterVolumeShadowSamplingEnabled();
        m_RenderDiagnostics.HPWaterSurfaceShadowSamplingEnabled =
            m_DeferredRenderer.IsHPWaterSurfaceShadowSamplingEnabled();
        m_RenderDiagnostics.HPWaterShadowCascadeDitherEnabled =
            m_DeferredRenderer.IsHPWaterShadowCascadeDitherEnabled();
        m_RenderDiagnostics.HPWaterVolumeShadowParamsEnabled =
            m_DeferredRenderer.IsHPWaterVolumeShadowParamsEnabled();
        m_RenderDiagnostics.HPWaterVolumePunctualLightLoopEnabled =
            m_DeferredRenderer.IsHPWaterVolumePunctualLightLoopEnabled();
        m_RenderDiagnostics.HPWaterVolumePointLightCount =
            m_DeferredRenderer.GetHPWaterVolumePointLightCount();
        m_RenderDiagnostics.HPWaterVolumeSpotLightCount =
            m_DeferredRenderer.GetHPWaterVolumeSpotLightCount();
        m_RenderDiagnostics.HPWaterVolumeShadowSoftness =
            m_DeferredRenderer.GetHPWaterVolumeShadowSoftness();
        m_RenderDiagnostics.HPWaterVolumeShadowMinFilterSize =
            m_DeferredRenderer.GetHPWaterVolumeShadowMinFilterSize();
        m_RenderDiagnostics.HPWaterVolumeShadowBlockerSamples =
            m_DeferredRenderer.GetHPWaterVolumeShadowBlockerSamples();
        m_RenderDiagnostics.HPWaterVolumeShadowFilterSamples =
            m_DeferredRenderer.GetHPWaterVolumeShadowFilterSamples();
        m_RenderDiagnostics.HPWaterVolumeSampleCount =
            m_DeferredRenderer.GetHPWaterVolumeSampleCount();
        m_RenderDiagnostics.HPWaterVolumeTemporalBlendFactor =
            m_DeferredRenderer.GetHPWaterVolumeTemporalBlendFactor();
        m_RenderDiagnostics.HPWaterVolumeSpatialFilterEnabled =
            m_DeferredRenderer.IsHPWaterVolumeSpatialFilterEnabled();
        m_RenderDiagnostics.HPWaterVolumeSpatialFilterIterations =
            m_DeferredRenderer.GetHPWaterVolumeSpatialFilterIterations();
        m_RenderDiagnostics.HPWaterVolumeMotionVectorsEnabled =
            m_DeferredRenderer.IsHPWaterVolumeMotionVectorsEnabled();
        m_RenderDiagnostics.HPWaterVolumeMotionVectorVelocityScale =
            m_DeferredRenderer.GetHPWaterVolumeMotionVectorVelocityScale();
        m_RenderDiagnostics.HPWaterVolumeTemporalDepthRejectionEnabled =
            m_DeferredRenderer.IsHPWaterVolumeTemporalDepthRejectionEnabled();
        m_RenderDiagnostics.HPWaterVolumeTemporalDepthThreshold =
            m_DeferredRenderer.GetHPWaterVolumeTemporalDepthThreshold();
        m_RenderDiagnostics.HPWaterVolumeSpatialDepthAwareEnabled =
            m_DeferredRenderer.IsHPWaterVolumeSpatialDepthAwareEnabled();
        m_RenderDiagnostics.HPWaterVolumeSpatialDepthSensitivity =
            m_DeferredRenderer.GetHPWaterVolumeSpatialDepthSensitivity();
        const glm::mat4 previousWaterVP = m_HasPreviousHPWaterViewProjection
            ? m_PreviousHPWaterViewProjection
            : viewProjection;
        m_RenderDiagnostics.HPWaterVolumeTemporalRan =
            m_RenderDiagnostics.HPWaterVolumeRan &&
            m_DeferredRenderer.TemporalFilterHPWaterVolume(viewProjection,
                                                           previousWaterVP,
                                                           hpWaterVolumeTemporalBlendFactor,
                                                           hpWaterVolumeMotionVectorsEnabled,
                                                           hpWaterVolumeMotionVectorVelocityScale,
                                                           hpWaterVolumeTemporalDepthRejection,
                                                           hpWaterVolumeTemporalDepthThreshold);
        m_RenderDiagnostics.HPWaterVolumeFilterRan =
            m_RenderDiagnostics.HPWaterVolumeRan &&
            m_DeferredRenderer.FilterHPWaterVolume(hpWaterVolumeSpatialFilterEnabled,
                                                   hpWaterVolumeSpatialFilterIterations,
                                                   hpWaterVolumeSpatialDepthAware,
                                                   hpWaterVolumeSpatialDepthSensitivity);
        m_RenderDiagnostics.HPWaterVolumeHistoryValid = m_DeferredRenderer.HasHPWaterVolumeHistory();
        m_RenderDiagnostics.HPWaterVolumeFilterIterations = m_DeferredRenderer.GetHPWaterVolumeFilterIterations();
        m_RenderDiagnostics.HPWaterVolumeUpsampleRan =
            m_RenderDiagnostics.HPWaterVolumeRan &&
            (!hpWaterVolumeSpatialFilterEnabled || m_RenderDiagnostics.HPWaterVolumeFilterRan) &&
            m_DeferredRenderer.UpsampleHPWaterVolume(nearClip, farClip);
        m_RenderDiagnostics.HPWaterCompositeRan =
            m_DeferredRenderer.CompositeHPWater(nearClip,
                                                farClip,
                                                hpWaterRefractionStrength,
                                                hpWaterWaterDispersionStrength,
                                                hpWaterMaxRefractionCrossDistance,
                                                hpWaterRefractionThicknessOffset,
                                                static_cast<int>(hpWaterRefractionSampleCount),
                                                hpWaterRefractionJitter,
                                                hpWaterFrameIndex,
                                                hpWaterEnvironmentReflectionIntensity,
                                                hpWaterIndirectLightStrength,
                                                hpWaterThinSSSStrength,
                                                hpWaterBacklitTransmissionStrength,
                                                hpWaterForwardScatterStrength,
                                                hpWaterForwardScatterBlurDensity,
                                                hpWaterMultiScatterScale,
                                                hpWaterPhaseG,
                                                hpWaterSpecularFGDStrength,
                                                hpWaterGGXEnergyCompensation,
                                                cameraPos,
                                                lightDir,
                                                lightColor,
                                                lightIntensity,
                                                numPointLights,
                                                pointPositions,
                                                pointColors,
                                                pointIntensities,
                                                pointRanges,
                                                numSpotLights,
                                                spotPositions,
                                                spotDirections,
                                                spotColors,
                                                spotIntensities,
                                                spotRanges,
                                                spotInnerCos,
                                                spotOuterCos,
                                                cameraView,
                                                hpWaterCascadeVP,
                                                hpWaterCascadeSplits,
                                                hpWaterShadowDepthTextureArray,
                                                hpWaterShadowDepthResolution,
                                                ps.ShadowsEnabled && m_ShadowMap.IsInitialized(),
                                                shadowSettings.DepthBias,
                                                shadowSettings.NormalBias,
                                                shadowSettings.PCFQuality,
                                                shadowSettings.CascadeBlendWidth,
                                                indirectSkyColor,
                                                indirectGroundColor,
                                                indirectTint,
                                                ps.IndirectLightingEnabled,
                                                ps.IndirectDiffuseIntensity,
                                                ps.SkyReflectionIntensity,
                                                ps.SSREnabled,
                                                ps.SSRMaxSteps,
                                                ps.SSRStepSize,
                                                ps.SSRThickness,
                                                ps.SSRMaxDistance,
                                                hpWaterSkyTexture,
                                                hpWaterReflectionProbeTexture,
                                                hpWaterReflectionProbeSecondaryTexture,
                                                hpWaterReflectionProbeBound,
                                                hpWaterReflectionProbeIntensity,
                                                hpWaterReflectionProbeBlend,
                                                hpWaterReflectionProbeHierarchyWeight,
                                                hpWaterReflectionProbeCenter,
                                                hpWaterReflectionProbeBoxSize,
                                                hpWaterReflectionProbeSecondaryTexture != 0
                                                    ? hpWaterReflectionProbeSecondaryCenter
                                                    : hpWaterReflectionProbeCenter,
                                                hpWaterReflectionProbeSecondaryTexture != 0
                                                    ? hpWaterReflectionProbeSecondaryBoxSize
                                                    : hpWaterReflectionProbeBoxSize,
                                                viewProjection,
                                                inverseViewProjection);
        if (m_RenderDiagnostics.HPWaterCompositeRan)
            m_RenderDiagnostics.HPWaterDrawn = m_RenderDiagnostics.HPWaterGBufferDrawn;

        m_PreviousHPWaterViewProjection = viewProjection;
        m_HasPreviousHPWaterViewProjection = true;
    } else {
        m_HasPreviousHPWaterViewProjection = false;
        m_DeferredRenderer.InvalidateHPWaterVolumeHistory();
    }

    m_RenderDiagnostics.HPWaterCompositeTexture = m_DeferredRenderer.GetHPWaterCompositeTexture();
    m_RenderDiagnostics.HPWaterRefractionDataTexture = m_DeferredRenderer.GetHPWaterRefractionDataTexture();
    m_RenderDiagnostics.HPWaterRefractionMetaTexture = m_DeferredRenderer.GetHPWaterRefractionMetaTexture();
    m_RenderDiagnostics.HPWaterRefractionNDCMarchEnabled =
        m_DeferredRenderer.IsHPWaterRefractionNDCMarchEnabled();
    m_RenderDiagnostics.HPWaterMaskTexture = m_DeferredRenderer.GetHPWaterMaskTexture();
    m_RenderDiagnostics.HPWaterMaskWidth = m_DeferredRenderer.GetWidth();
    m_RenderDiagnostics.HPWaterMaskHeight = m_DeferredRenderer.GetHeight();
    m_RenderDiagnostics.HPWaterDepthPyramidTexture = m_DeferredRenderer.GetHPWaterDepthPyramidTexture();
    m_RenderDiagnostics.HPWaterDepthPyramidMipCount = m_DeferredRenderer.GetHPWaterDepthPyramidMipCount();
    m_RenderDiagnostics.HPWaterDepthPyramidWidth = m_DeferredRenderer.GetWidth();
    m_RenderDiagnostics.HPWaterDepthPyramidHeight = m_DeferredRenderer.GetHeight();
    m_RenderDiagnostics.HPWaterDepthMergedToSceneDepth =
        m_DeferredRenderer.IsHPWaterDepthMergedToSceneDepth();
    m_RenderDiagnostics.HPWaterNormalMergedToSceneGBuffer =
        m_DeferredRenderer.IsHPWaterNormalMergedToSceneGBuffer();
    m_RenderDiagnostics.HPWaterStencilMarkedInSceneDepth =
        m_DeferredRenderer.IsHPWaterStencilMarkedInSceneDepth();
    m_RenderDiagnostics.HPWaterStencilRef = m_DeferredRenderer.GetHPWaterStencilRef();
    m_RenderDiagnostics.HPWaterForwardScatterMipEnabled = m_DeferredRenderer.IsHPWaterSceneColorMipValid();
    m_RenderDiagnostics.HPWaterForwardScatterMipCount = m_DeferredRenderer.GetHPWaterSceneColorMipCount();
    if (m_RenderDiagnostics.HPWaterGBufferDrawn == 0) {
        m_RenderDiagnostics.HPWaterIndirectScatterIntegrationEnabled = false;
        m_RenderDiagnostics.HPWaterBSDFComponentWeightingEnabled = false;
    }
    m_RenderDiagnostics.HPWaterVolumeColorTexture = m_DeferredRenderer.GetHPWaterVolumeTexture(0);
    m_RenderDiagnostics.HPWaterVolumeTransmittanceTexture = m_DeferredRenderer.GetHPWaterVolumeTexture(1);
    m_RenderDiagnostics.HPWaterVolumeDepthTexture = m_DeferredRenderer.GetHPWaterVolumeTexture(2);
    m_RenderDiagnostics.HPWaterVolumeWidth = m_DeferredRenderer.GetHPWaterVolumeWidth();
    m_RenderDiagnostics.HPWaterVolumeHeight = m_DeferredRenderer.GetHPWaterVolumeHeight();
    m_RenderDiagnostics.HPWaterVolumeTemporalNeighborhoodClampEnabled =
        m_DeferredRenderer.IsHPWaterVolumeTemporalNeighborhoodClampEnabled();
    m_RenderDiagnostics.HPWaterVolumeTemporalMotionReprojectionEnabled =
        m_DeferredRenderer.IsHPWaterVolumeTemporalMotionReprojectionEnabled();
    m_RenderDiagnostics.HPWaterVolumeExplicitMotionVectorEnabled =
        m_DeferredRenderer.IsHPWaterVolumeExplicitMotionVectorEnabled();
    m_RenderDiagnostics.HPWaterVolumeShadowSamplingEnabled =
        m_DeferredRenderer.IsHPWaterVolumeShadowSamplingEnabled();
    m_RenderDiagnostics.HPWaterSurfaceShadowSamplingEnabled =
        m_DeferredRenderer.IsHPWaterSurfaceShadowSamplingEnabled();
    m_RenderDiagnostics.HPWaterShadowCascadeDitherEnabled =
        m_DeferredRenderer.IsHPWaterShadowCascadeDitherEnabled();
    m_RenderDiagnostics.HPWaterVolumeShadowParamsEnabled =
        m_DeferredRenderer.IsHPWaterVolumeShadowParamsEnabled();
    m_RenderDiagnostics.HPWaterVolumePunctualLightLoopEnabled =
        m_DeferredRenderer.IsHPWaterVolumePunctualLightLoopEnabled();
    m_RenderDiagnostics.HPWaterVolumePointLightCount =
        m_DeferredRenderer.GetHPWaterVolumePointLightCount();
    m_RenderDiagnostics.HPWaterVolumeSpotLightCount =
        m_DeferredRenderer.GetHPWaterVolumeSpotLightCount();
    m_RenderDiagnostics.HPWaterVolumeShadowSoftness =
        m_DeferredRenderer.GetHPWaterVolumeShadowSoftness();
    m_RenderDiagnostics.HPWaterVolumeShadowMinFilterSize =
        m_DeferredRenderer.GetHPWaterVolumeShadowMinFilterSize();
    m_RenderDiagnostics.HPWaterVolumeShadowBlockerSamples =
        m_DeferredRenderer.GetHPWaterVolumeShadowBlockerSamples();
    m_RenderDiagnostics.HPWaterVolumeShadowFilterSamples =
        m_DeferredRenderer.GetHPWaterVolumeShadowFilterSamples();
    m_RenderDiagnostics.HPWaterVolumeMotionVectorTexture =
        m_DeferredRenderer.GetHPWaterVolumeMotionVectorTexture();
    m_RenderDiagnostics.HPWaterVolumeTemporalBlendFactor =
        m_DeferredRenderer.GetHPWaterVolumeTemporalBlendFactor();
    m_RenderDiagnostics.HPWaterVolumeSpatialFilterEnabled =
        m_DeferredRenderer.IsHPWaterVolumeSpatialFilterEnabled();
    m_RenderDiagnostics.HPWaterVolumeSpatialFilterIterations =
        m_DeferredRenderer.GetHPWaterVolumeSpatialFilterIterations();
    m_RenderDiagnostics.HPWaterVolumeMotionVectorsEnabled =
        m_DeferredRenderer.IsHPWaterVolumeMotionVectorsEnabled();
    m_RenderDiagnostics.HPWaterVolumeMotionVectorVelocityScale =
        m_DeferredRenderer.GetHPWaterVolumeMotionVectorVelocityScale();
    m_RenderDiagnostics.HPWaterVolumeTemporalDepthRejectionEnabled =
        m_DeferredRenderer.IsHPWaterVolumeTemporalDepthRejectionEnabled();
    m_RenderDiagnostics.HPWaterVolumeTemporalDepthThreshold =
        m_DeferredRenderer.GetHPWaterVolumeTemporalDepthThreshold();
    m_RenderDiagnostics.HPWaterVolumeSpatialDepthAwareEnabled =
        m_DeferredRenderer.IsHPWaterVolumeSpatialDepthAwareEnabled();
    m_RenderDiagnostics.HPWaterVolumeSpatialDepthSensitivity =
        m_DeferredRenderer.GetHPWaterVolumeSpatialDepthSensitivity();
    m_RenderDiagnostics.HPWaterVolumeTemporalNeighborhoodClampStrength =
        m_DeferredRenderer.GetHPWaterVolumeTemporalNeighborhoodClampStrength();
    m_RenderDiagnostics.HPWaterVolumeHistoryValid = m_DeferredRenderer.HasHPWaterVolumeHistory();
    m_RenderDiagnostics.HPWaterVolumeHistoryColorTexture = m_DeferredRenderer.GetHPWaterVolumeHistoryTexture(0);
    m_RenderDiagnostics.HPWaterVolumeHistoryTransmittanceTexture = m_DeferredRenderer.GetHPWaterVolumeHistoryTexture(1);
    m_RenderDiagnostics.HPWaterVolumeHistoryDepthTexture = m_DeferredRenderer.GetHPWaterVolumeHistoryTexture(2);
    m_RenderDiagnostics.HPWaterVolumeFilterIterations = m_DeferredRenderer.GetHPWaterVolumeFilterIterations();
    m_RenderDiagnostics.HPWaterVolumeFilteredColorTexture = m_DeferredRenderer.GetHPWaterVolumeFilteredTexture(0);
    m_RenderDiagnostics.HPWaterVolumeFilteredTransmittanceTexture = m_DeferredRenderer.GetHPWaterVolumeFilteredTexture(1);
    m_RenderDiagnostics.HPWaterVolumeFilteredDepthTexture = m_DeferredRenderer.GetHPWaterVolumeFilteredTexture(2);
    m_RenderDiagnostics.HPWaterVolumeUpsampledColorTexture = m_DeferredRenderer.GetHPWaterVolumeUpsampledTexture(0);
    m_RenderDiagnostics.HPWaterVolumeUpsampledTransmittanceTexture = m_DeferredRenderer.GetHPWaterVolumeUpsampledTexture(1);
    m_RenderDiagnostics.HPWaterVolumeUpsampledDepthTexture = m_DeferredRenderer.GetHPWaterVolumeUpsampledTexture(2);
    m_RenderDiagnostics.HPWaterVolumeUpsampledWidth = m_DeferredRenderer.GetWidth();
    m_RenderDiagnostics.HPWaterVolumeUpsampledHeight = m_DeferredRenderer.GetHeight();
    m_RenderDiagnostics.HPWaterCausticValid = m_DeferredRenderer.IsHPWaterCausticValid();
    m_RenderDiagnostics.HPWaterCausticTexture = m_DeferredRenderer.GetHPWaterCausticTexture();
    m_RenderDiagnostics.HPWaterCausticComputeTexture =
        m_DeferredRenderer.GetHPWaterCausticComputeIrradianceTexture();
    m_RenderDiagnostics.HPWaterCausticComputeWidth =
        m_DeferredRenderer.GetHPWaterCausticComputeWidth();
    m_RenderDiagnostics.HPWaterCausticComputeHeight =
        m_DeferredRenderer.GetHPWaterCausticComputeHeight();
    m_RenderDiagnostics.HPWaterCausticComputeValid =
        m_DeferredRenderer.IsHPWaterCausticComputeIrradianceValid();
    m_RenderDiagnostics.HPWaterCausticComputeRan =
        m_DeferredRenderer.DidRunHPWaterCausticComputeIrradiance();
    m_RenderDiagnostics.HPWaterCausticComputeAtomicEnabled =
        m_DeferredRenderer.IsHPWaterCausticComputeAtomicEnabled();
    m_RenderDiagnostics.HPWaterCausticComputeAtomicTexture =
        m_DeferredRenderer.GetHPWaterCausticComputeAtomicTexture();
    m_RenderDiagnostics.HPWaterCausticShadowDepthConsumed =
        m_DeferredRenderer.IsHPWaterCausticShadowDepthConsumed();
    m_RenderDiagnostics.HPWaterCausticRGBReceiverProjectionEnabled =
        m_DeferredRenderer.IsHPWaterCausticRGBReceiverProjectionEnabled();
    m_RenderDiagnostics.HPWaterCausticExponentialLightStepsEnabled =
        m_DeferredRenderer.IsHPWaterCausticExponentialLightStepsEnabled();
    m_RenderDiagnostics.HPWaterCausticFrameDitherEnabled =
        m_DeferredRenderer.IsHPWaterCausticFrameDitherEnabled();
    m_RenderDiagnostics.HPWaterCausticAtlasReceiverOutputEnabled =
        m_DeferredRenderer.IsHPWaterCausticAtlasReceiverOutputEnabled();
    m_RenderDiagnostics.HPWaterCausticCascadeBlendEnabled =
        m_DeferredRenderer.IsHPWaterCausticCascadeBlendEnabled();
    m_RenderDiagnostics.HPWaterCausticAtlasEdgeFilterEnabled =
        m_DeferredRenderer.IsHPWaterCausticAtlasEdgeFilterEnabled();
    m_RenderDiagnostics.HPWaterCausticSpectralWeightingEnabled =
        m_DeferredRenderer.IsHPWaterCausticSpectralWeightingEnabled();
    m_RenderDiagnostics.HPWaterCausticFilteredValid = m_DeferredRenderer.IsHPWaterCausticFilteredValid();
    m_RenderDiagnostics.HPWaterCausticFilteredTexture = m_DeferredRenderer.GetHPWaterCausticFilteredTexture();
    m_RenderDiagnostics.HPWaterCausticFilterIterations = m_DeferredRenderer.GetHPWaterCausticFilterIterations();
    m_RenderDiagnostics.HPWaterCausticFilterComputeParityEnabled =
        m_DeferredRenderer.IsHPWaterCausticFilterComputeParityEnabled();
    m_RenderDiagnostics.HPWaterCausticFilterLDSHaloEnabled =
        m_DeferredRenderer.IsHPWaterCausticFilterLDSHaloEnabled();
    m_RenderDiagnostics.HPWaterCausticAtlasValid = m_DeferredRenderer.IsHPWaterCausticAtlasValid();
    m_RenderDiagnostics.HPWaterCausticAtlasTexture = m_DeferredRenderer.GetHPWaterCausticAtlasTexture();
    m_RenderDiagnostics.HPWaterCausticAtlasDepthTexture = m_DeferredRenderer.GetHPWaterCausticAtlasDepthTexture();
    m_RenderDiagnostics.HPWaterCausticAtlasTileResolution = m_DeferredRenderer.GetHPWaterCausticAtlasTileResolution();
    m_RenderDiagnostics.HPWaterCausticAtlasWidth = m_DeferredRenderer.GetHPWaterCausticAtlasWidth();
    m_RenderDiagnostics.HPWaterCausticAtlasHeight = m_DeferredRenderer.GetHPWaterCausticAtlasHeight();
    m_RenderDiagnostics.HPWaterCausticAtlasCascades = m_DeferredRenderer.GetHPWaterCausticAtlasCascadeCount();
    m_RenderDiagnostics.HPWaterCausticAtlasDrawn = hpWaterCausticAtlasDrawn;
    m_RenderDiagnostics.HPWaterCausticAtlasConsumed = m_DeferredRenderer.IsHPWaterCausticAtlasConsumed();
    m_RenderDiagnostics.HPWaterFluidDynamicsValid = m_DeferredRenderer.IsHPWaterFluidDynamicsValid();
    m_RenderDiagnostics.HPWaterFluidComputeRan = m_DeferredRenderer.DidHPWaterFluidComputeRun();
    m_RenderDiagnostics.HPWaterFluidEdgeAbsorptionParityEnabled =
        m_DeferredRenderer.IsHPWaterFluidEdgeAbsorptionParityEnabled();
    m_RenderDiagnostics.HPWaterFluidSourceClampEnabled = m_DeferredRenderer.IsHPWaterFluidSourceClampEnabled();
    m_RenderDiagnostics.HPWaterFluidWaveEquationParityEnabled =
        m_DeferredRenderer.IsHPWaterFluidWaveEquationParityEnabled();
    m_RenderDiagnostics.HPWaterFluidSampleClampParityEnabled =
        m_DeferredRenderer.IsHPWaterFluidDynamicsValid();
    m_RenderDiagnostics.HPWaterFluidHeightCaptureCacheReused =
        m_DeferredRenderer.WasHPWaterFluidHeightCaptureCacheReused();
    m_RenderDiagnostics.HPWaterFluidLayerFilteringParityEnabled =
        hpWaterFluidEnabled ? m_RenderDiagnostics.HPWaterFluidLayerFilteringParityEnabled : false;
    m_RenderDiagnostics.HPWaterFluidHeightTexture = m_DeferredRenderer.GetHPWaterFluidHeightTexture();
    m_RenderDiagnostics.HPWaterFluidResolution = m_DeferredRenderer.GetHPWaterFluidResolution();
    m_RenderDiagnostics.HPWaterFluidWaveSpeed = hpWaterFluidWaveSpeed;
    m_RenderDiagnostics.HPWaterFluidDamping = hpWaterFluidDamping;
    m_RenderDiagnostics.HPWaterFluidObstacleValid = m_DeferredRenderer.IsHPWaterFluidObstacleValid();
    m_RenderDiagnostics.HPWaterFluidObstacleTexture = m_DeferredRenderer.GetHPWaterFluidObstacleTexture();
    m_RenderDiagnostics.HPWaterFluidHeightFieldValid = m_DeferredRenderer.IsHPWaterFluidHeightFieldValid();
    m_RenderDiagnostics.HPWaterFluidHeightCaptureRan = m_DeferredRenderer.DidHPWaterFluidHeightCaptureRun();
    m_RenderDiagnostics.HPWaterFluidHeightCaptureValid = m_DeferredRenderer.IsHPWaterFluidHeightCaptureValid();
    m_RenderDiagnostics.HPWaterFluidWaterHeightTexture = m_DeferredRenderer.GetHPWaterFluidWaterHeightTexture();
    m_RenderDiagnostics.HPWaterFluidSceneHeightTexture = m_DeferredRenderer.GetHPWaterFluidSceneHeightTexture();
    m_RenderDiagnostics.DeferredOutputTexture = m_DeferredRenderer.GetOutputTexture();
}

// ── Terrain Rendering ───────────────────────────────────────────────

void Scene::OnRenderTerrain(const glm::mat4& viewProjection, const glm::vec3& cameraPos) {
    auto terrainView = m_Registry.view<TransformComponent, TerrainComponent>();
    for (auto entity : terrainView) {
        if (!IsEntityActiveInHierarchy(entity)) continue;
        auto& tc = terrainView.get<TransformComponent>(entity);
        auto& terrain = terrainView.get<TerrainComponent>(entity);

        // Lazy generation
        if (terrain._NeedsRebuild || !terrain._Terrain) {
            terrain._Terrain = std::make_shared<Terrain>();
            if (!terrain.HeightmapPath.empty()) {
                terrain._Terrain->GenerateFromImage(terrain.HeightmapPath,
                    terrain.WorldSizeX, terrain.WorldSizeZ, terrain.HeightScale);
            } else {
                terrain._Terrain->GenerateProcedural(terrain.Resolution,
                    terrain.WorldSizeX, terrain.WorldSizeZ, terrain.HeightScale,
                    terrain.Octaves, terrain.Persistence, terrain.Lacunarity,
                    terrain.NoiseScale, terrain.Seed);
            }
            terrain._Mesh = terrain._Terrain->GetMesh();
            terrain._NeedsRebuild = false;

            // Load textures
            for (int i = 0; i < 4; i++) {
                if (!terrain.LayerTexturePaths[i].empty())
                    terrain._LayerTextures[i] = Texture2D::Create(terrain.LayerTexturePaths[i]);
            }
        }

        if (!terrain._Mesh) continue;

        // Load terrain shader
        static std::shared_ptr<Shader> s_TerrainShader;
        if (!s_TerrainShader) {
            s_TerrainShader = Shader::CreateFromFile("shaders/Terrain.shader");
            if (!s_TerrainShader) {
                VE_ENGINE_ERROR("Failed to load Terrain.shader");
                continue;
            }
        }

        glm::mat4 model = GetWorldTransform(entity);
        glm::mat4 mvp = viewProjection * model;

        s_TerrainShader->Bind();
        s_TerrainShader->SetMat4("u_MVP", mvp);
        s_TerrainShader->SetMat4("u_Model", model);
        s_TerrainShader->SetVec4("u_EntityColor", glm::vec4(1.0f));

        // Lighting — find directional light
        glm::vec3 lightDir = glm::normalize(glm::vec3(0.3f, 1.0f, 0.5f));
        glm::vec3 lightColor(1.0f);
        float lightIntensity = 1.0f;
        {
            auto lightView = m_Registry.view<DirectionalLightComponent>();
            for (auto le : lightView) {
                if (!IsEntityActiveInHierarchy(le)) continue;
                auto& dl = lightView.get<DirectionalLightComponent>(le);
                lightDir = GetEntityForward(le);
                lightColor = glm::vec3(dl.Color[0], dl.Color[1], dl.Color[2]);
                lightIntensity = dl.Intensity;
                break;
            }
        }
        s_TerrainShader->SetVec3("u_LightDir", lightDir);
        s_TerrainShader->SetVec3("u_LightColor", lightColor);
        s_TerrainShader->SetFloat("u_LightIntensity", lightIntensity);
        s_TerrainShader->SetVec3("u_ViewPos", cameraPos);
        s_TerrainShader->SetFloat("u_Roughness", terrain.Roughness);

        // Point lights
        static constexpr int MAX_PL = 8;
        int numPL = 0;
        auto plView = m_Registry.view<TransformComponent, PointLightComponent>();
        for (auto plE : plView) {
            if (numPL >= MAX_PL) break;
            if (!IsEntityActiveInHierarchy(plE)) continue;
            auto [ptc, pl] = plView.get<TransformComponent, PointLightComponent>(plE);
            glm::vec3 plPos = glm::vec3(GetWorldTransform(plE)[3]);
            s_TerrainShader->SetVec3(s_PointLightPositions[numPL], plPos);
            s_TerrainShader->SetVec3(s_PointLightColors[numPL],
                glm::vec3(pl.Color[0], pl.Color[1], pl.Color[2]));
            s_TerrainShader->SetFloat(s_PointLightIntensities[numPL], pl.Intensity);
            s_TerrainShader->SetFloat(s_PointLightRanges[numPL], pl.Range);
            numPL++;
        }
        s_TerrainShader->SetInt("u_NumPointLights", numPL);

        // Spot lights for terrain
        static constexpr int MAX_SL = 4;
        int numSL = 0;
        auto slView = m_Registry.view<TransformComponent, SpotLightComponent>();
        for (auto slE : slView) {
            if (numSL >= MAX_SL) break;
            if (!IsEntityActiveInHierarchy(slE)) continue;
            auto [stc, sl] = slView.get<TransformComponent, SpotLightComponent>(slE);
            glm::mat4 wm = GetWorldTransform(slE);
            glm::vec3 slPos = glm::vec3(wm[3]);
            glm::vec3 localDir = glm::normalize(glm::vec3(sl.Direction[0], sl.Direction[1], sl.Direction[2]));
            glm::vec3 slDir = glm::normalize(glm::mat3(wm) * localDir);
            s_TerrainShader->SetVec3(s_SpotLightPositions[numSL], slPos);
            s_TerrainShader->SetVec3(s_SpotLightDirections[numSL], slDir);
            s_TerrainShader->SetVec3(s_SpotLightColors[numSL],
                glm::vec3(sl.Color[0], sl.Color[1], sl.Color[2]));
            s_TerrainShader->SetFloat(s_SpotLightIntensities[numSL], sl.Intensity);
            s_TerrainShader->SetFloat(s_SpotLightRanges[numSL], sl.Range);
            s_TerrainShader->SetFloat(s_SpotLightInnerCos[numSL],
                std::cos(glm::radians(sl.InnerAngle)));
            s_TerrainShader->SetFloat(s_SpotLightOuterCos[numSL],
                std::cos(glm::radians(sl.OuterAngle)));
            numSL++;
        }
        s_TerrainShader->SetInt("u_NumSpotLights", numSL);

        // Blend heights and tiling
        s_TerrainShader->SetFloat("u_BlendHeight0", terrain.BlendHeights[0]);
        s_TerrainShader->SetFloat("u_BlendHeight1", terrain.BlendHeights[1]);
        s_TerrainShader->SetFloat("u_BlendHeight2", terrain.BlendHeights[2]);
        s_TerrainShader->SetFloat("u_HeightScale", terrain.HeightScale);
        s_TerrainShader->SetFloat("u_Tiling0", terrain.LayerTiling[0]);
        s_TerrainShader->SetFloat("u_Tiling1", terrain.LayerTiling[1]);
        s_TerrainShader->SetFloat("u_Tiling2", terrain.LayerTiling[2]);
        s_TerrainShader->SetFloat("u_Tiling3", terrain.LayerTiling[3]);

        // Bind layer textures
        for (int i = 0; i < 4; i++) {
            s_TerrainShader->SetInt(s_TerrainLayers[i], i);
            if (terrain._LayerTextures[i])
                terrain._LayerTextures[i]->Bind(i);
        }


        RenderCommand::DrawIndexed(terrain._Mesh);
    }
}

// ── Decal Rendering ─────────────────────────────────────────────────

void Scene::OnRenderDecals(const glm::mat4& viewProjection, const glm::mat4& viewMatrix,
                           const glm::mat4& projMatrix, uint32_t depthTexture,
                           uint32_t screenWidth, uint32_t screenHeight) {
    auto view = m_Registry.view<TransformComponent, DecalComponent>();

    // Collect decals and sort by SortOrder
    struct DecalEntry {
        entt::entity Entity;
        int SortOrder;
    };
    std::vector<DecalEntry> decals;
    decals.reserve(view.size_hint());
    for (auto entity : view) {
        if (!IsEntityActiveInHierarchy(entity)) continue;
        auto& dc = view.get<DecalComponent>(entity);
        decals.push_back({ entity, dc.SortOrder });
    }
    if (decals.empty()) return;

    std::sort(decals.begin(), decals.end(),
        [](const DecalEntry& a, const DecalEntry& b) { return a.SortOrder < b.SortOrder; });

    // Get decal shader
    auto decalShader = ShaderLibrary::Get("Decal");
    if (!decalShader) return;

    // Get cube mesh for decal volume
    auto cubeMesh = MeshLibrary::GetCube();
    if (!cubeMesh) return;

    // Compute inverse VP
    glm::mat4 invVP = glm::inverse(viewProjection);

    // Setup GL state for decals
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);   // don't depth-test (we reconstruct from depth texture)
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT);       // render back faces
    glDepthMask(GL_FALSE);      // don't write depth

    // Bind depth texture to unit 7
    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D, depthTexture);

    decalShader->Bind();
    decalShader->SetInt("u_DepthTexture", 7);
    decalShader->SetMat4("u_InvVP", invVP);
    decalShader->SetVec4("u_ScreenSize", glm::vec4((float)screenWidth, (float)screenHeight, 0.0f, 0.0f));

    for (auto& entry : decals) {
        auto& tc = m_Registry.get<TransformComponent>(entry.Entity);
        auto& dc = m_Registry.get<DecalComponent>(entry.Entity);

        // Build model matrix with decal size baked in
        glm::mat4 worldTransform = GetWorldTransform(entry.Entity);
        glm::mat4 sizeScale = glm::scale(glm::mat4(1.0f),
            glm::vec3(dc.Size[0], dc.Size[1], dc.Size[2]));
        glm::mat4 model = worldTransform * sizeScale;
        glm::mat4 mvp = viewProjection * model;
        glm::mat4 invModel = glm::inverse(model);

        decalShader->SetMat4("u_MVP", mvp);
        decalShader->SetMat4("u_Model", model);
        decalShader->SetMat4("u_InvModel", invModel);
        decalShader->SetVec4("u_EntityColor",
            glm::vec4(dc.Color[0], dc.Color[1], dc.Color[2], dc.Color[3]));
        decalShader->SetFloat("u_NormalBlend", dc.NormalBlend);
        decalShader->SetFloat("u_FadeDistance", dc.FadeDistance);

        // Bind decal texture
        if (dc._Texture) {
            glActiveTexture(GL_TEXTURE0);
            dc._Texture->Bind(0);
            decalShader->SetInt("u_HasMainTex", 1);
            decalShader->SetInt("u_UseTexture", 0);
        } else {
            decalShader->SetInt("u_HasMainTex", 0);
            decalShader->SetInt("u_UseTexture", 0);
        }

        RenderCommand::DrawIndexed(cubeMesh);
    }

    // Restore GL state
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glCullFace(GL_BACK);
    glDisable(GL_BLEND);
}

// ── Sprite Rendering ────────────────────────────────────────────────

void Scene::OnRenderSprites(const glm::mat4& viewProjection) {
    auto view = m_Registry.view<TransformComponent, SpriteRendererComponent>();

    // Collect and sort by SortingOrder, then by Z position
    struct SpriteEntry {
        entt::entity Entity;
        int SortingOrder;
        float Z;
    };
    std::vector<SpriteEntry> sprites;
    sprites.reserve(view.size_hint());

    for (auto e : view) {
        if (!IsEntityActiveInHierarchy(e)) continue;
        auto& sr = view.get<SpriteRendererComponent>(e);
        glm::mat4 wt = GetWorldTransform(e);
        sprites.push_back({ e, sr.SortingOrder, wt[3].z });
    }

    std::sort(sprites.begin(), sprites.end(), [](const SpriteEntry& a, const SpriteEntry& b) {
        if (a.SortingOrder != b.SortingOrder) return a.SortingOrder < b.SortingOrder;
        return a.Z < b.Z;
    });

    SpriteBatchRenderer::BeginBatch(viewProjection);
    for (auto& entry : sprites) {
        auto& sr = m_Registry.get<SpriteRendererComponent>(entry.Entity);
        glm::mat4 transform = GetWorldTransform(entry.Entity);
        glm::vec4 color(sr.Color[0], sr.Color[1], sr.Color[2], sr.Color[3]);
        SpriteBatchRenderer::DrawSprite(transform, color, sr.Texture, sr.UVRect);
    }
    SpriteBatchRenderer::EndBatch();
}

// ── Sprite Animation ────────────────────────────────────────────────

void Scene::StartSpriteAnimations() {
    auto view = m_Registry.view<SpriteAnimatorComponent>();
    for (auto e : view) {
        auto& sa = view.get<SpriteAnimatorComponent>(e);
        if (sa.PlayOnStart) {
            sa._Playing = true;
            sa._CurrentFrame = sa.StartFrame;
            sa._Timer = 0.0f;
        }
    }
}

void Scene::StopSpriteAnimations() {
    auto view = m_Registry.view<SpriteAnimatorComponent>();
    for (auto e : view) {
        auto& sa = view.get<SpriteAnimatorComponent>(e);
        sa._Playing = false;
        sa._Timer = 0.0f;
        sa._CurrentFrame = sa.StartFrame;
    }
}

// ── Particle System ─────────────────────────────────────────────────

static thread_local std::mt19937 s_RNG{ std::random_device{}() };

static float RandomFloat(float min, float max) {
    std::uniform_real_distribution<float> dist(min, max);
    return dist(s_RNG);
}

void Scene::StartParticles() {
    auto view = m_Registry.view<ParticleSystemComponent>();
    for (auto e : view) {
        auto& ps = view.get<ParticleSystemComponent>(e);
        ps._Particles.resize(ps.MaxParticles);
        for (auto& p : ps._Particles)
            p.Active = false;
        ps._EmissionAccumulator = 0.0f;
        ps._Playing = ps.PlayOnStart;
        ps._EmissionStopped = false;
    }
    VE_ENGINE_INFO("Particles started");
}

void Scene::StopParticles() {
    auto view = m_Registry.view<ParticleSystemComponent>();
    for (auto e : view) {
        auto& ps = view.get<ParticleSystemComponent>(e);
        ps._Particles.clear();
        ps._EmissionAccumulator = 0.0f;
        ps._Playing = false;
        ps._EmissionStopped = false;
    }
    VE_ENGINE_INFO("Particles stopped");
}

// Helper: generate a random direction on the unit sphere
static glm::vec3 RandomOnSphere() {
    float theta = RandomFloat(0.0f, 2.0f * 3.14159265f);
    float phi = std::acos(RandomFloat(-1.0f, 1.0f));
    float sp = std::sin(phi);
    return glm::vec3(sp * std::cos(theta), sp * std::sin(theta), std::cos(phi));
}

// Helper: generate a random direction within a cone (half-angle in radians) pointing up (+Y)
static glm::vec3 RandomInCone(float halfAngleRad) {
    float theta = RandomFloat(0.0f, 2.0f * 3.14159265f);
    float cosAngle = std::cos(halfAngleRad);
    float z = RandomFloat(cosAngle, 1.0f);
    float r = std::sqrt(1.0f - z * z);
    return glm::vec3(r * std::cos(theta), z, r * std::sin(theta));
}

void Scene::OnUpdateParticles(float dt) {
    auto view = m_Registry.view<TransformComponent, ParticleSystemComponent>();
    for (auto entity : view) {
        if (!IsEntityActiveInHierarchy(entity)) continue;
        auto& tc = view.get<TransformComponent>(entity);
        auto& ps = view.get<ParticleSystemComponent>(entity);
        if (!ps._Playing) continue;

        // Resize pool if MaxParticles changed at runtime
        if ((int)ps._Particles.size() != ps.MaxParticles) {
            ps._Particles.resize(ps.MaxParticles);
            for (auto& p : ps._Particles)
                p.Active = false;
        }

        glm::vec3 gravity(ps.Gravity[0], ps.Gravity[1], ps.Gravity[2]);
        glm::mat4 worldMat = GetWorldTransform(entity);
        glm::vec3 emitterPos = glm::vec3(worldMat[3]);

        // 1) Age existing particles, apply gravity, lerp color/size
        bool anyActive = false;
        for (auto& p : ps._Particles) {
            if (!p.Active) continue;
            p.Lifetime += dt;
            if (p.Lifetime >= p.MaxLife) {
                p.Active = false;
                continue;
            }
            anyActive = true;
            p.Velocity += gravity * dt;
            p.Position += p.Velocity * dt;

            float t = p.Lifetime / p.MaxLife;
            glm::vec4 sc(ps.StartColor[0], ps.StartColor[1], ps.StartColor[2], ps.StartColor[3]);
            glm::vec4 ec(ps.EndColor[0], ps.EndColor[1], ps.EndColor[2], ps.EndColor[3]);
            p.Color = glm::mix(sc, ec, t);
            p.Size = glm::mix(ps.StartSize, ps.EndSize, t);
        }

        // Non-looping: stop emitting once we have cycled; stop playing when all dead
        if (!ps.Looping && ps._EmissionStopped) {
            if (!anyActive)
                ps._Playing = false;
            continue;
        }

        // 2) Emit new particles via accumulator
        ps._EmissionAccumulator += ps.EmissionRate * dt;
        while (ps._EmissionAccumulator >= 1.0f) {
            ps._EmissionAccumulator -= 1.0f;
            // Find inactive slot
            bool emitted = false;
            for (auto& p : ps._Particles) {
                if (p.Active) continue;

                p.Active = true;
                p.Lifetime = 0.0f;
                p.MaxLife = ps.ParticleLifetime + RandomFloat(-ps.LifetimeVariance, ps.LifetimeVariance);
                if (p.MaxLife < 0.01f) p.MaxLife = 0.01f;
                p.Size = ps.StartSize;
                p.Color = glm::vec4(ps.StartColor[0], ps.StartColor[1], ps.StartColor[2], ps.StartColor[3]);

                // Spawn position and velocity based on emitter shape
                switch (ps.Shape) {
                    case EmitterShape::Point:
                        p.Position = emitterPos;
                        p.Velocity = glm::vec3(
                            RandomFloat(ps.VelocityMin[0], ps.VelocityMax[0]),
                            RandomFloat(ps.VelocityMin[1], ps.VelocityMax[1]),
                            RandomFloat(ps.VelocityMin[2], ps.VelocityMax[2]));
                        break;

                    case EmitterShape::Sphere: {
                        // Spawn at random point on sphere surface, velocity outward
                        glm::vec3 dir = RandomOnSphere();
                        float radius = RandomFloat(0.0f, ps.ShapeRadius);
                        p.Position = emitterPos + dir * radius;
                        float speed = RandomFloat(ps.SpeedMin, ps.SpeedMax);
                        p.Velocity = dir * speed;
                        break;
                    }

                    case EmitterShape::Cone: {
                        // Spawn at emitter, velocity within cone pointing up (+Y)
                        float halfAngle = glm::radians(ps.ConeAngle);
                        glm::vec3 dir = RandomInCone(halfAngle);
                        // Offset spawn position slightly within cone base radius
                        float baseRadius = ps.ShapeRadius * RandomFloat(0.0f, 1.0f);
                        glm::vec3 offset(
                            baseRadius * std::cos(RandomFloat(0.0f, 6.28318f)),
                            0.0f,
                            baseRadius * std::sin(RandomFloat(0.0f, 6.28318f)));
                        p.Position = emitterPos + offset;
                        float speed = RandomFloat(ps.SpeedMin, ps.SpeedMax);
                        p.Velocity = dir * speed;
                        break;
                    }
                }

                emitted = true;
                break;
            }

            // If pool is full and not looping, mark emission as done
            if (!emitted && !ps.Looping) {
                ps._EmissionStopped = true;
                break;
            }
        }
    }
}

void Scene::OnRenderParticles(const glm::mat4& viewProjection, const glm::vec3& cameraPos) {
    auto view = m_Registry.view<ParticleSystemComponent>();

    bool anyActive = false;
    for (auto e : view) {
        if (!IsEntityActiveInHierarchy(e)) continue;
        auto& ps = view.get<ParticleSystemComponent>(e);
        if (!ps._Playing) continue;
        for (auto& p : ps._Particles) {
            if (p.Active) { anyActive = true; break; }
        }
        if (anyActive) break;
    }
    if (!anyActive) return;

    ParticleRenderer::BeginBatch(viewProjection);

    for (auto entity : view) {
        if (!IsEntityActiveInHierarchy(entity)) continue;
        auto& ps = view.get<ParticleSystemComponent>(entity);
        if (!ps._Playing) continue;

        // DrawParticles handles back-to-front sorting and billboard rendering
        ParticleRenderer::DrawParticles(ps._Particles, cameraPos, ps.Texture);
    }

    ParticleRenderer::EndBatch();
}

// ── Video Playback ────────────────────────────────────────────────────

void Scene::StartVideo() {
    auto view = m_Registry.view<VideoPlayerComponent>();
    for (auto entity : view) {
        auto& vc = view.get<VideoPlayerComponent>(entity);
        if (vc.VideoPath.empty()) continue;

        if (!vc._Player) {
            vc._Player = std::make_shared<VideoPlayer>();
        }

        if (!vc._Player->IsOpen()) {
            if (!vc._Player->Open(vc.VideoPath)) continue;
        }

        vc._Player->SetLooping(vc.Loop);

        if (vc.PlayOnAwake) {
            vc._Player->Play();
        }
    }
    VE_ENGINE_INFO("Video playback started");
}

void Scene::StopVideo() {
    auto view = m_Registry.view<VideoPlayerComponent>();
    for (auto entity : view) {
        auto& vc = view.get<VideoPlayerComponent>(entity);
        if (vc._Player) {
            vc._Player->Close();
            vc._Player.reset();
        }
    }
    VE_ENGINE_INFO("Video playback stopped");
}

void Scene::UpdateVideo(float deltaTime) {
    auto view = m_Registry.view<VideoPlayerComponent>();
    for (auto entity : view) {
        if (!IsEntityActiveInHierarchy(entity)) continue;
        auto& vc = view.get<VideoPlayerComponent>(entity);
        if (vc._Player && vc._Player->IsPlaying()) {
            vc._Player->Update(deltaTime);
        }
    }
}

// ── Runtime UI Rendering ──────────────────────────────────────────────

void Scene::OnRenderUI(uint32_t screenWidth, uint32_t screenHeight,
                       float mouseX, float mouseY, bool mouseDown) {
    // Check if any UICanvasComponent exists
    auto canvasView = m_Registry.view<UICanvasComponent>();
    bool hasCanvas = false;
    for (auto e : canvasView) { (void)e; hasCanvas = true; break; }
    if (!hasCanvas) return;

    UIRenderer::SetMouseState(mouseX, mouseY, mouseDown);
    UIRenderer::BeginFrame(screenWidth, screenHeight);

    // Helper to compute screen position from anchor + offset
    auto ComputeScreenPos = [&](const UIRectTransformComponent& rt) -> glm::vec2 {
        float anchorX = 0.0f, anchorY = 0.0f;
        switch (rt.Anchor) {
            case UIAnchorType::TopLeft:      anchorX = 0;                      anchorY = 0; break;
            case UIAnchorType::TopCenter:    anchorX = screenWidth * 0.5f;     anchorY = 0; break;
            case UIAnchorType::TopRight:     anchorX = (float)screenWidth;     anchorY = 0; break;
            case UIAnchorType::MiddleLeft:   anchorX = 0;                      anchorY = screenHeight * 0.5f; break;
            case UIAnchorType::Center:       anchorX = screenWidth * 0.5f;     anchorY = screenHeight * 0.5f; break;
            case UIAnchorType::MiddleRight:  anchorX = (float)screenWidth;     anchorY = screenHeight * 0.5f; break;
            case UIAnchorType::BottomLeft:   anchorX = 0;                      anchorY = (float)screenHeight; break;
            case UIAnchorType::BottomCenter: anchorX = screenWidth * 0.5f;     anchorY = (float)screenHeight; break;
            case UIAnchorType::BottomRight:  anchorX = (float)screenWidth;     anchorY = (float)screenHeight; break;
        }
        float x = anchorX + rt.AnchoredPosition[0] - rt.Pivot[0] * rt.Size[0];
        float y = anchorY + rt.AnchoredPosition[1] - rt.Pivot[1] * rt.Size[1];
        return { x, y };
    };

    // First pass: update button states
    auto buttonView = m_Registry.view<UIRectTransformComponent, UIButtonComponent>();
    for (auto entity : buttonView) {
        if (!IsEntityActiveInHierarchy(entity)) continue;
        auto& rt = buttonView.get<UIRectTransformComponent>(entity);
        auto& btn = buttonView.get<UIButtonComponent>(entity);
        auto pos = ComputeScreenPos(rt);
        btn._Hovered = UIRenderer::IsMouseOver(pos.x, pos.y, rt.Size[0], rt.Size[1]);
        btn._Clicked = UIRenderer::IsMouseClicked(pos.x, pos.y, rt.Size[0], rt.Size[1]);
        btn._Pressed = btn._Hovered && mouseDown;
    }

    // Second pass: render UI elements (images, then text on top)
    // Render UIImageComponents
    auto imageView = m_Registry.view<UIRectTransformComponent, UIImageComponent>();
    for (auto entity : imageView) {
        if (!IsEntityActiveInHierarchy(entity)) continue;
        auto& rt = imageView.get<UIRectTransformComponent>(entity);
        auto& img = imageView.get<UIImageComponent>(entity);
        auto pos = ComputeScreenPos(rt);

        glm::vec4 color = { img.Color[0], img.Color[1], img.Color[2], img.Color[3] };

        // If this entity also has a button, use button color
        if (m_Registry.all_of<UIButtonComponent>(entity)) {
            auto& btn = m_Registry.get<UIButtonComponent>(entity);
            const auto& c = btn._Pressed ? btn.PressedColor :
                            btn._Hovered ? btn.HoverColor : btn.NormalColor;
            color = { c[0], c[1], c[2], c[3] };
        }

        if (img._Texture)
            UIRenderer::DrawImage(pos.x, pos.y, rt.Size[0], rt.Size[1], img._Texture, color);
        else
            UIRenderer::DrawRect(pos.x, pos.y, rt.Size[0], rt.Size[1], color);
    }

    // Render buttons without images (colored rect)
    auto btnOnlyView = m_Registry.view<UIRectTransformComponent, UIButtonComponent>();
    for (auto entity : btnOnlyView) {
        if (!IsEntityActiveInHierarchy(entity)) continue;
        if (m_Registry.all_of<UIImageComponent>(entity)) continue; // already drawn
        auto& rt = btnOnlyView.get<UIRectTransformComponent>(entity);
        auto& btn = btnOnlyView.get<UIButtonComponent>(entity);
        auto pos = ComputeScreenPos(rt);
        const auto& c = btn._Pressed ? btn.PressedColor :
                        btn._Hovered ? btn.HoverColor : btn.NormalColor;
        UIRenderer::DrawRect(pos.x, pos.y, rt.Size[0], rt.Size[1],
                             { c[0], c[1], c[2], c[3] });
    }

    // Render button labels (centered text inside button rect)
    {
        auto btnLabelView = m_Registry.view<UIRectTransformComponent, UIButtonComponent>();
        for (auto entity : btnLabelView) {
            if (!IsEntityActiveInHierarchy(entity)) continue;
            auto& rt = btnLabelView.get<UIRectTransformComponent>(entity);
            auto& btn = btnLabelView.get<UIButtonComponent>(entity);
            if (btn.Label.empty()) continue;

            auto font = FontLibrary::GetDefault();
            if (!font) continue;

            float textW = font->MeasureTextWidth(btn.Label) * (btn.FontSize / font->GetPixelHeight());
            float textH = btn.FontSize;
            auto pos = ComputeScreenPos(rt);
            float tx = pos.x + (rt.Size[0] - textW) * 0.5f;
            float ty = pos.y + (rt.Size[1] - textH) * 0.5f;
            UIRenderer::DrawText(btn.Label, tx, ty, btn.FontSize,
                                 { btn.LabelColor[0], btn.LabelColor[1], btn.LabelColor[2], btn.LabelColor[3] },
                                 font);
        }
    }

    // Render UITextComponents
    auto textView = m_Registry.view<UIRectTransformComponent, UITextComponent>();
    for (auto entity : textView) {
        if (!IsEntityActiveInHierarchy(entity)) continue;
        auto& rt = textView.get<UIRectTransformComponent>(entity);
        auto& txt = textView.get<UITextComponent>(entity);
        auto pos = ComputeScreenPos(rt);

        // Lazy-load font
        if (!txt._Font) {
            if (!txt.FontPath.empty())
                txt._Font = FontAtlas::Create(txt.FontPath, txt.FontSize);
            if (!txt._Font)
                txt._Font = FontLibrary::GetDefault();
        }

        UIRenderer::DrawText(txt.Text, pos.x, pos.y, txt.FontSize,
                             { txt.Color[0], txt.Color[1], txt.Color[2], txt.Color[3] },
                             txt._Font);
    }

    UIRenderer::EndFrame();
}

} // namespace VE
