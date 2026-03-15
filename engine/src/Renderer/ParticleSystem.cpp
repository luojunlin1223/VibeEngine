/*
 * ParticleSystem — Batched particle renderer with back-to-front sorting.
 *
 * Uses a dedicated Particle.shader with soft circular falloff for untextured
 * particles. Shares the same vertex format as SpriteBatchRenderer:
 * Position(3) + Color(4) + TexCoord(2) + TexIndex(1) = 10 floats per vertex.
 */
#include "VibeEngine/Renderer/ParticleSystem.h"
#include "VibeEngine/Scene/Components.h"
#include "VibeEngine/Core/Log.h"

#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <numeric>

namespace VE {

struct ParticleVertex {
    glm::vec3 Position;
    glm::vec4 Color;
    glm::vec2 TexCoord;
    float     TexIndex;
};

static constexpr uint32_t MaxQuads    = 10000;
static constexpr uint32_t MaxVertices = MaxQuads * 4;
static constexpr uint32_t MaxIndices  = MaxQuads * 6;
static constexpr uint32_t MaxTextureSlots = 16;

struct ParticleBatchData {
    std::shared_ptr<VertexArray>  VAO;
    std::shared_ptr<VertexBuffer> VBO;
    std::shared_ptr<IndexBuffer>  IBO;
    std::shared_ptr<Shader>       ParticleShader;
    std::shared_ptr<Texture2D>    WhiteTexture;

    ParticleVertex* VertexBufferBase = nullptr;
    ParticleVertex* VertexBufferPtr  = nullptr;
    uint32_t        IndexCount = 0;

    std::array<std::shared_ptr<Texture2D>, MaxTextureSlots> TextureSlots;
    uint32_t TextureSlotIndex = 1; // slot 0 = white texture

    glm::mat4 ViewProjection = glm::mat4(1.0f);
    glm::vec3 CameraPos = glm::vec3(0.0f);

    // Cached billboard basis vectors (recomputed per-frame in BeginBatch is not needed;
    // we compute per-particle for correct billboarding from all angles)

    ParticleRenderer::Stats RenderStats;
};

static ParticleBatchData s_Data;

// Temporary index array for sorting (avoids per-frame allocation for common sizes)
static std::vector<uint32_t> s_SortIndices;

void ParticleRenderer::Init() {
    // White 1x1 texture (slot 0 fallback)
    uint32_t whitePixel = 0xFFFFFFFF;
    s_Data.WhiteTexture = Texture2D::Create(1, 1, &whitePixel);
    s_Data.TextureSlots[0] = s_Data.WhiteTexture;

    // Create VAO first (IBO binding is part of VAO state)
    s_Data.VAO = VertexArray::Create();
    s_Data.VAO->Bind();

    // Dynamic vertex buffer
    s_Data.VBO = VertexBuffer::Create(nullptr, MaxVertices * sizeof(ParticleVertex));
    s_Data.VBO->SetLayout({
        { ShaderDataType::Float3, "a_Position" },
        { ShaderDataType::Float4, "a_Color" },
        { ShaderDataType::Float2, "a_TexCoord" },
        { ShaderDataType::Float,  "a_TexIndex" },
    });

    // CPU-side vertex staging buffer
    s_Data.VertexBufferBase = new ParticleVertex[MaxVertices];

    // Pre-compute index buffer (0,1,2, 2,3,0, 4,5,6, ...)
    auto indices = new uint32_t[MaxIndices];
    uint32_t offset = 0;
    for (uint32_t i = 0; i < MaxIndices; i += 6) {
        indices[i + 0] = offset + 0;
        indices[i + 1] = offset + 1;
        indices[i + 2] = offset + 2;
        indices[i + 3] = offset + 2;
        indices[i + 4] = offset + 3;
        indices[i + 5] = offset + 0;
        offset += 4;
    }
    s_Data.IBO = IndexBuffer::Create(indices, MaxIndices);
    delete[] indices;

    s_Data.VAO->AddVertexBuffer(s_Data.VBO);
    s_Data.VAO->SetIndexBuffer(s_Data.IBO);

    // Load dedicated particle shader (falls back to Sprite shader)
    s_Data.ParticleShader = Shader::CreateFromFile("shaders/Particle.shader");
    if (!s_Data.ParticleShader) {
        VE_ENGINE_WARN("Particle.shader not found, falling back to Sprite.shader");
        s_Data.ParticleShader = Shader::CreateFromFile("shaders/Sprite.shader");
    }
    if (s_Data.ParticleShader) {
        s_Data.ParticleShader->SetName("Particle");
        s_Data.ParticleShader->Bind();
        for (int i = 0; i < (int)MaxTextureSlots; i++)
            s_Data.ParticleShader->SetInt("u_Textures[" + std::to_string(i) + "]", i);
        s_Data.ParticleShader->Unbind();
    } else {
        VE_ENGINE_ERROR("Failed to load any particle shader");
    }

    // Clean up GL state
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

void ParticleRenderer::Shutdown() {
    delete[] s_Data.VertexBufferBase;
    s_Data.VertexBufferBase = nullptr;
    s_Data.VertexBufferPtr  = nullptr;
    s_Data.VAO.reset();
    s_Data.VBO.reset();
    s_Data.IBO.reset();
    s_Data.ParticleShader.reset();
    s_Data.WhiteTexture.reset();
    for (auto& slot : s_Data.TextureSlots)
        slot.reset();
    s_SortIndices.clear();
    s_SortIndices.shrink_to_fit();
}

void ParticleRenderer::BeginBatch(const glm::mat4& viewProjection) {
    s_Data.ViewProjection   = viewProjection;
    s_Data.VertexBufferPtr  = s_Data.VertexBufferBase;
    s_Data.IndexCount       = 0;
    s_Data.TextureSlotIndex = 1; // keep slot 0 as white
}

void ParticleRenderer::Flush() {
    if (s_Data.IndexCount == 0) return;

    uint32_t dataSize = static_cast<uint32_t>(
        reinterpret_cast<uint8_t*>(s_Data.VertexBufferPtr) -
        reinterpret_cast<uint8_t*>(s_Data.VertexBufferBase));
    s_Data.VBO->SetData(s_Data.VertexBufferBase, dataSize);

    // Bind textures
    for (uint32_t i = 0; i < s_Data.TextureSlotIndex; i++) {
        if (s_Data.TextureSlots[i])
            s_Data.TextureSlots[i]->Bind(i);
    }

    // Alpha blending state
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE); // ZWrite Off — don't occlude other particles

    s_Data.ParticleShader->Bind();
    s_Data.ParticleShader->SetMat4("u_ViewProjection", s_Data.ViewProjection);

    s_Data.VAO->Bind();
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(s_Data.IndexCount), GL_UNSIGNED_INT, nullptr);

    // Restore state
    glDepthMask(GL_TRUE);

    s_Data.RenderStats.DrawCalls++;
}

void ParticleRenderer::EndBatch() {
    Flush();
}

void ParticleRenderer::EmitQuad(const glm::vec3& position, float size,
                                const glm::vec4& color, const glm::vec3& cameraPos,
                                float texIndex) {
    if (s_Data.IndexCount >= MaxIndices) {
        Flush();
        BeginBatch(s_Data.ViewProjection);
    }

    // Billboard: compute right/up vectors facing camera
    glm::vec3 toCamera = cameraPos - position;
    float dist = glm::length(toCamera);
    if (dist < 0.0001f)
        toCamera = glm::vec3(0.0f, 0.0f, 1.0f);
    else
        toCamera /= dist;

    glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
    if (std::abs(glm::dot(toCamera, worldUp)) > 0.999f)
        worldUp = glm::vec3(0.0f, 0.0f, 1.0f);

    glm::vec3 right = glm::normalize(glm::cross(worldUp, toCamera));
    glm::vec3 up    = glm::cross(toCamera, right);

    float halfSize = size * 0.5f;
    glm::vec3 bl = position - right * halfSize - up * halfSize;
    glm::vec3 br = position + right * halfSize - up * halfSize;
    glm::vec3 tr = position + right * halfSize + up * halfSize;
    glm::vec3 tl = position - right * halfSize + up * halfSize;

    s_Data.VertexBufferPtr->Position = bl;
    s_Data.VertexBufferPtr->Color    = color;
    s_Data.VertexBufferPtr->TexCoord = { 0.0f, 0.0f };
    s_Data.VertexBufferPtr->TexIndex = texIndex;
    s_Data.VertexBufferPtr++;

    s_Data.VertexBufferPtr->Position = br;
    s_Data.VertexBufferPtr->Color    = color;
    s_Data.VertexBufferPtr->TexCoord = { 1.0f, 0.0f };
    s_Data.VertexBufferPtr->TexIndex = texIndex;
    s_Data.VertexBufferPtr++;

    s_Data.VertexBufferPtr->Position = tr;
    s_Data.VertexBufferPtr->Color    = color;
    s_Data.VertexBufferPtr->TexCoord = { 1.0f, 1.0f };
    s_Data.VertexBufferPtr->TexIndex = texIndex;
    s_Data.VertexBufferPtr++;

    s_Data.VertexBufferPtr->Position = tl;
    s_Data.VertexBufferPtr->Color    = color;
    s_Data.VertexBufferPtr->TexCoord = { 0.0f, 1.0f };
    s_Data.VertexBufferPtr->TexIndex = texIndex;
    s_Data.VertexBufferPtr++;

    s_Data.IndexCount += 6;
    s_Data.RenderStats.QuadCount++;
}

void ParticleRenderer::DrawParticles(const std::vector<Particle>& particles,
                                     const glm::vec3& cameraPos,
                                     const std::shared_ptr<Texture2D>& texture) {
    // Find or assign texture slot
    float texIndex = 0.0f;
    if (texture) {
        for (uint32_t i = 1; i < s_Data.TextureSlotIndex; i++) {
            if (s_Data.TextureSlots[i]->GetNativeTextureID() == texture->GetNativeTextureID()) {
                texIndex = static_cast<float>(i);
                break;
            }
        }
        if (texIndex == 0.0f) {
            if (s_Data.TextureSlotIndex >= MaxTextureSlots) {
                Flush();
                BeginBatch(s_Data.ViewProjection);
            }
            texIndex = static_cast<float>(s_Data.TextureSlotIndex);
            s_Data.TextureSlots[s_Data.TextureSlotIndex] = texture;
            s_Data.TextureSlotIndex++;
        }
    }

    // Collect active particle indices
    uint32_t activeCount = 0;
    if (s_SortIndices.size() < particles.size())
        s_SortIndices.resize(particles.size());

    for (uint32_t i = 0; i < (uint32_t)particles.size(); i++) {
        if (particles[i].Active)
            s_SortIndices[activeCount++] = i;
    }

    if (activeCount == 0) return;

    // Sort back-to-front (farthest from camera first) for correct alpha blending
    std::sort(s_SortIndices.begin(), s_SortIndices.begin() + activeCount,
        [&particles, &cameraPos](uint32_t a, uint32_t b) {
            float distA = glm::dot(particles[a].Position - cameraPos, particles[a].Position - cameraPos);
            float distB = glm::dot(particles[b].Position - cameraPos, particles[b].Position - cameraPos);
            return distA > distB; // farthest first
        });

    // Emit sorted billboard quads
    for (uint32_t i = 0; i < activeCount; i++) {
        const Particle& p = particles[s_SortIndices[i]];
        EmitQuad(p.Position, p.Size, p.Color, cameraPos, texIndex);
    }
}

ParticleRenderer::Stats ParticleRenderer::GetStats() {
    return s_Data.RenderStats;
}

void ParticleRenderer::ResetStats() {
    s_Data.RenderStats = {};
}

} // namespace VE
