/*
 * SpriteBatchRenderer implementation.
 *
 * Uses a dynamic VBO with pre-computed index buffer to batch
 * up to 10,000 quads per draw call. Texture slots (max 16) are
 * managed per-batch with automatic flushing.
 */
#include "VibeEngine/Renderer/SpriteBatchRenderer.h"
#include "VibeEngine/Renderer/VertexArray.h"
#include "VibeEngine/Renderer/Buffer.h"
#include "VibeEngine/Renderer/Shader.h"
#include "VibeEngine/Core/Log.h"

#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>

namespace VE {

struct SpriteVertex {
    glm::vec3 Position;
    glm::vec4 Color;
    glm::vec2 TexCoord;
    float     TexIndex;
};

static constexpr uint32_t MaxQuads    = 10000;
static constexpr uint32_t MaxVertices = MaxQuads * 4;
static constexpr uint32_t MaxIndices  = MaxQuads * 6;
static constexpr uint32_t MaxTextureSlots = 16;

struct BatchData {
    std::shared_ptr<VertexArray>  VAO;
    std::shared_ptr<VertexBuffer> VBO;
    std::shared_ptr<IndexBuffer>  IBO;
    std::shared_ptr<Shader>       SpriteShader;
    std::shared_ptr<Texture2D>    WhiteTexture;

    SpriteVertex* VertexBufferBase = nullptr;
    SpriteVertex* VertexBufferPtr  = nullptr;
    uint32_t      IndexCount = 0;

    std::array<std::shared_ptr<Texture2D>, MaxTextureSlots> TextureSlots;
    uint32_t TextureSlotIndex = 1; // slot 0 = white texture

    glm::mat4 ViewProjection = glm::mat4(1.0f);

    SpriteBatchRenderer::Stats RenderStats;
};

static BatchData s_Data;

void SpriteBatchRenderer::Init() {
    // White 1x1 texture (slot 0 fallback)
    uint32_t whitePixel = 0xFFFFFFFF;
    s_Data.WhiteTexture = Texture2D::Create(1, 1, &whitePixel);
    s_Data.TextureSlots[0] = s_Data.WhiteTexture;

    // Create and BIND VAO first — GL_ELEMENT_ARRAY_BUFFER binding is part of
    // VAO state. IndexBuffer::Create() binds GL_ELEMENT_ARRAY_BUFFER, which
    // would corrupt whatever VAO was previously active (e.g. sky sphere).
    // We must ensure our sprite VAO is bound before creating the IBO.
    s_Data.VAO = VertexArray::Create();
    s_Data.VAO->Bind();

    // Dynamic vertex buffer
    s_Data.VBO = VertexBuffer::Create(nullptr, MaxVertices * sizeof(SpriteVertex));
    s_Data.VBO->SetLayout({
        { ShaderDataType::Float3, "a_Position" },
        { ShaderDataType::Float4, "a_Color" },
        { ShaderDataType::Float2, "a_TexCoord" },
        { ShaderDataType::Float,  "a_TexIndex" },
    });

    // CPU-side vertex staging buffer
    s_Data.VertexBufferBase = new SpriteVertex[MaxVertices];

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

    // Load sprite shader
    s_Data.SpriteShader = Shader::CreateFromFile("shaders/Sprite.shader");
    if (!s_Data.SpriteShader) {
        VE_ENGINE_ERROR("Failed to load Sprite.shader");
        return;
    }
    s_Data.SpriteShader->SetName("Sprite");

    // Set texture sampler uniforms
    s_Data.SpriteShader->Bind();
    for (int i = 0; i < (int)MaxTextureSlots; i++)
        s_Data.SpriteShader->SetInt("u_Textures[" + std::to_string(i) + "]", i);
    s_Data.SpriteShader->Unbind();

    // Clean up GL state — leave no VAO/VBO/IBO bound
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

void SpriteBatchRenderer::Shutdown() {
    delete[] s_Data.VertexBufferBase;
    s_Data.VertexBufferBase = nullptr;
    s_Data.VertexBufferPtr  = nullptr;
    s_Data.VAO.reset();
    s_Data.VBO.reset();
    s_Data.IBO.reset();
    s_Data.SpriteShader.reset();
    s_Data.WhiteTexture.reset();
    for (auto& slot : s_Data.TextureSlots)
        slot.reset();
}

void SpriteBatchRenderer::BeginBatch(const glm::mat4& viewProjection) {
    s_Data.ViewProjection   = viewProjection;
    s_Data.VertexBufferPtr  = s_Data.VertexBufferBase;
    s_Data.IndexCount       = 0;
    s_Data.TextureSlotIndex = 1; // keep slot 0 as white
}

void SpriteBatchRenderer::Flush() {
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

    // Set render state for transparent sprites
    glDepthMask(GL_FALSE); // ZWrite Off

    s_Data.SpriteShader->Bind();
    s_Data.SpriteShader->SetMat4("u_ViewProjection", s_Data.ViewProjection);

    s_Data.VAO->Bind();
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(s_Data.IndexCount), GL_UNSIGNED_INT, nullptr);

    // Restore state
    glDepthMask(GL_TRUE);

    s_Data.RenderStats.DrawCalls++;
}

void SpriteBatchRenderer::EndBatch() {
    Flush();
}

void SpriteBatchRenderer::DrawSprite(const glm::mat4& transform,
                                      const glm::vec4& color,
                                      const std::shared_ptr<Texture2D>& texture,
                                      const std::array<float, 4>& uvRect) {
    if (s_Data.IndexCount >= MaxIndices)
        Flush(), BeginBatch(s_Data.ViewProjection);

    // Find or assign texture slot
    float texIndex = 0.0f;
    if (texture) {
        // Search existing slots
        for (uint32_t i = 1; i < s_Data.TextureSlotIndex; i++) {
            if (s_Data.TextureSlots[i]->GetNativeTextureID() == texture->GetNativeTextureID()) {
                texIndex = static_cast<float>(i);
                break;
            }
        }
        if (texIndex == 0.0f) {
            if (s_Data.TextureSlotIndex >= MaxTextureSlots)
                Flush(), BeginBatch(s_Data.ViewProjection);
            texIndex = static_cast<float>(s_Data.TextureSlotIndex);
            s_Data.TextureSlots[s_Data.TextureSlotIndex] = texture;
            s_Data.TextureSlotIndex++;
        }
    }

    // UV coordinates from rect (x, y, w, h)
    float u0 = uvRect[0];
    float v0 = uvRect[1];
    float u1 = uvRect[0] + uvRect[2];
    float v1 = uvRect[1] + uvRect[3];

    // Quad corners in local space (centered at origin)
    constexpr glm::vec4 positions[4] = {
        { -0.5f, -0.5f, 0.0f, 1.0f },
        {  0.5f, -0.5f, 0.0f, 1.0f },
        {  0.5f,  0.5f, 0.0f, 1.0f },
        { -0.5f,  0.5f, 0.0f, 1.0f },
    };

    glm::vec2 texCoords[4] = {
        { u0, v0 },
        { u1, v0 },
        { u1, v1 },
        { u0, v1 },
    };

    for (int i = 0; i < 4; i++) {
        glm::vec4 worldPos = transform * positions[i];
        s_Data.VertexBufferPtr->Position = glm::vec3(worldPos);
        s_Data.VertexBufferPtr->Color    = color;
        s_Data.VertexBufferPtr->TexCoord = texCoords[i];
        s_Data.VertexBufferPtr->TexIndex = texIndex;
        s_Data.VertexBufferPtr++;
    }

    s_Data.IndexCount += 6;
    s_Data.RenderStats.QuadCount++;
}

SpriteBatchRenderer::Stats SpriteBatchRenderer::GetStats() {
    return s_Data.RenderStats;
}

void SpriteBatchRenderer::ResetStats() {
    s_Data.RenderStats = {};
}

} // namespace VE
