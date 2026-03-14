/*
 * UIRenderer implementation — screen-space batched UI rendering.
 *
 * Uses the same batching technique as SpriteBatchRenderer but with a
 * screen-space orthographic projection (origin top-left, Y-down).
 * Reuses the Sprite shader format for vertex layout.
 */
#include "VibeEngine/UI/UIRenderer.h"
#include "VibeEngine/Renderer/VertexArray.h"
#include "VibeEngine/Renderer/Buffer.h"
#include "VibeEngine/Renderer/Shader.h"
#include "VibeEngine/Renderer/RenderCommand.h"
#include "VibeEngine/Core/Log.h"

#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>
#include <array>

namespace VE {

struct UIVertex {
    glm::vec3 Position;
    glm::vec4 Color;
    glm::vec2 TexCoord;
    float     TexIndex;
};

static constexpr uint32_t MaxQuads        = 10000;
static constexpr uint32_t MaxVertices     = MaxQuads * 4;
static constexpr uint32_t MaxIndices      = MaxQuads * 6;
static constexpr uint32_t MaxTextureSlots = 16;

struct UIBatchData {
    std::shared_ptr<VertexArray>  VAO;
    std::shared_ptr<VertexBuffer> VBO;
    std::shared_ptr<IndexBuffer>  IBO;
    std::shared_ptr<Shader>       UIShader;
    std::shared_ptr<Texture2D>    WhiteTexture;

    UIVertex* VertexBufferBase = nullptr;
    UIVertex* VertexBufferPtr  = nullptr;
    uint32_t  IndexCount = 0;

    std::array<std::shared_ptr<Texture2D>, MaxTextureSlots> TextureSlots;
    uint32_t TextureSlotIndex = 1; // slot 0 = white

    glm::mat4 Projection = glm::mat4(1.0f);
    uint32_t ScreenWidth = 1280;
    uint32_t ScreenHeight = 720;

    // Mouse state
    float MouseX = 0, MouseY = 0;
    bool  MouseDown = false;
    bool  MouseWasDown = false; // previous frame

    UIRenderer::Stats RenderStats;
};

static UIBatchData s_UI;

void UIRenderer::Init() {
    // White 1x1 fallback
    uint32_t white = 0xFFFFFFFF;
    s_UI.WhiteTexture = Texture2D::Create(1, 1, &white);
    s_UI.TextureSlots[0] = s_UI.WhiteTexture;

    s_UI.VAO = VertexArray::Create();
    s_UI.VAO->Bind();

    s_UI.VBO = VertexBuffer::Create(nullptr, MaxVertices * sizeof(UIVertex));
    s_UI.VBO->SetLayout({
        { ShaderDataType::Float3, "a_Position" },
        { ShaderDataType::Float4, "a_Color" },
        { ShaderDataType::Float2, "a_TexCoord" },
        { ShaderDataType::Float,  "a_TexIndex" },
    });

    s_UI.VertexBufferBase = new UIVertex[MaxVertices];

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
    s_UI.IBO = IndexBuffer::Create(indices, MaxIndices);
    delete[] indices;

    s_UI.VAO->AddVertexBuffer(s_UI.VBO);
    s_UI.VAO->SetIndexBuffer(s_UI.IBO);

    // Reuse Sprite shader (same vertex format)
    s_UI.UIShader = Shader::CreateFromFile("shaders/Sprite.shader");
    if (!s_UI.UIShader) {
        VE_ENGINE_ERROR("UIRenderer: Failed to load Sprite.shader for UI");
        return;
    }
    s_UI.UIShader->SetName("UI");

    s_UI.UIShader->Bind();
    for (int i = 0; i < (int)MaxTextureSlots; i++)
        s_UI.UIShader->SetInt("u_Textures[" + std::to_string(i) + "]", i);
    s_UI.UIShader->Unbind();

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    // Initialize default font
    FontLibrary::GetDefault();

    VE_ENGINE_INFO("UIRenderer initialized");
}

void UIRenderer::Shutdown() {
    delete[] s_UI.VertexBufferBase;
    s_UI.VertexBufferBase = nullptr;
    s_UI.VertexBufferPtr  = nullptr;
    s_UI.VAO.reset();
    s_UI.VBO.reset();
    s_UI.IBO.reset();
    s_UI.UIShader.reset();
    s_UI.WhiteTexture.reset();
    for (auto& slot : s_UI.TextureSlots) slot.reset();
    FontLibrary::Clear();
}

void UIRenderer::SetMouseState(float mouseX, float mouseY, bool mouseDown) {
    s_UI.MouseWasDown = s_UI.MouseDown;
    s_UI.MouseX = mouseX;
    s_UI.MouseY = mouseY;
    s_UI.MouseDown = mouseDown;
}

void UIRenderer::BeginFrame(uint32_t screenWidth, uint32_t screenHeight) {
    s_UI.ScreenWidth  = screenWidth;
    s_UI.ScreenHeight = screenHeight;

    // Screen-space ortho: origin at top-left, Y goes down
    s_UI.Projection = glm::ortho(0.0f, static_cast<float>(screenWidth),
                                  static_cast<float>(screenHeight), 0.0f,
                                  -1.0f, 1.0f);

    s_UI.VertexBufferPtr  = s_UI.VertexBufferBase;
    s_UI.IndexCount       = 0;
    s_UI.TextureSlotIndex = 1;
    s_UI.RenderStats = {};
}

void UIRenderer::Flush() {
    if (s_UI.IndexCount == 0) return;

    uint32_t dataSize = static_cast<uint32_t>(
        reinterpret_cast<uint8_t*>(s_UI.VertexBufferPtr) -
        reinterpret_cast<uint8_t*>(s_UI.VertexBufferBase));
    s_UI.VBO->SetData(s_UI.VertexBufferBase, dataSize);

    for (uint32_t i = 0; i < s_UI.TextureSlotIndex; i++) {
        if (s_UI.TextureSlots[i])
            s_UI.TextureSlots[i]->Bind(i);
    }

    // Disable depth, enable blending
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    s_UI.UIShader->Bind();
    s_UI.UIShader->SetMat4("u_ViewProjection", s_UI.Projection);

    s_UI.VAO->Bind();
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(s_UI.IndexCount), GL_UNSIGNED_INT, nullptr);

    // Restore state
    glEnable(GL_DEPTH_TEST);

    s_UI.RenderStats.DrawCalls++;
}

void UIRenderer::StartNewBatch() {
    Flush();
    s_UI.VertexBufferPtr  = s_UI.VertexBufferBase;
    s_UI.IndexCount       = 0;
    s_UI.TextureSlotIndex = 1;
}

void UIRenderer::EndFrame() {
    Flush();
}

// Internal flush helper callable from file-local free functions
static void FlushAndReset() {
    UIRenderer::Flush();
    s_UI.VertexBufferPtr  = s_UI.VertexBufferBase;
    s_UI.IndexCount       = 0;
    s_UI.TextureSlotIndex = 1;
}

static float FindOrAssignTextureSlot(const std::shared_ptr<Texture2D>& texture) {
    if (!texture) return 0.0f;

    for (uint32_t i = 1; i < s_UI.TextureSlotIndex; i++) {
        if (s_UI.TextureSlots[i]->GetNativeTextureID() == texture->GetNativeTextureID())
            return static_cast<float>(i);
    }

    if (s_UI.TextureSlotIndex >= MaxTextureSlots)
        FlushAndReset();

    float idx = static_cast<float>(s_UI.TextureSlotIndex);
    s_UI.TextureSlots[s_UI.TextureSlotIndex] = texture;
    s_UI.TextureSlotIndex++;
    return idx;
}

static void PushQuad(float x, float y, float w, float h,
                     const glm::vec4& color, float texIndex,
                     float u0 = 0.0f, float v0 = 0.0f,
                     float u1 = 1.0f, float v1 = 1.0f) {
    if (s_UI.IndexCount >= MaxIndices)
        FlushAndReset();

    // Top-left origin, clockwise winding: TL, TR, BR, BL
    UIVertex* v = s_UI.VertexBufferPtr;
    v[0] = { {x,     y,     0.0f}, color, {u0, v0}, texIndex };
    v[1] = { {x + w, y,     0.0f}, color, {u1, v0}, texIndex };
    v[2] = { {x + w, y + h, 0.0f}, color, {u1, v1}, texIndex };
    v[3] = { {x,     y + h, 0.0f}, color, {u0, v1}, texIndex };
    s_UI.VertexBufferPtr += 4;
    s_UI.IndexCount += 6;
    s_UI.RenderStats.QuadCount++;
}

void UIRenderer::DrawRect(float x, float y, float width, float height,
                           const glm::vec4& color) {
    PushQuad(x, y, width, height, color, 0.0f);
}

void UIRenderer::DrawImage(float x, float y, float width, float height,
                            const std::shared_ptr<Texture2D>& texture,
                            const glm::vec4& tint) {
    float texIdx = FindOrAssignTextureSlot(texture);
    PushQuad(x, y, width, height, tint, texIdx);
}

void UIRenderer::DrawText(const std::string& text, float x, float y,
                           float fontSize,
                           const glm::vec4& color,
                           const std::shared_ptr<FontAtlas>& font) {
    auto f = font ? font : FontLibrary::GetDefault();
    if (!f || !f->GetAtlasTexture()) return;

    float texIdx = FindOrAssignTextureSlot(f->GetAtlasTexture());
    float scale = fontSize / f->GetPixelHeight();
    float curX = x;

    for (char c : text) {
        if (c == '\n') {
            curX = x;
            y += f->GetLineHeight() * scale;
            continue;
        }

        const auto& g = f->GetGlyph(c);
        if (g.Width <= 0.0f) {
            curX += g.Advance * scale;
            continue;
        }

        float gx = curX + g.BearingX * scale;
        float gy = y + (f->GetLineHeight() - g.BearingY) * scale;
        float gw = g.Width * scale;
        float gh = g.Height * scale;

        PushQuad(gx, gy, gw, gh, color, texIdx, g.U0, g.V0, g.U1, g.V1);
        curX += g.Advance * scale;
    }
}

bool UIRenderer::IsMouseOver(float x, float y, float width, float height) {
    return s_UI.MouseX >= x && s_UI.MouseX <= x + width &&
           s_UI.MouseY >= y && s_UI.MouseY <= y + height;
}

bool UIRenderer::IsMouseClicked(float x, float y, float width, float height) {
    return IsMouseOver(x, y, width, height) && s_UI.MouseDown && !s_UI.MouseWasDown;
}

UIRenderer::Stats UIRenderer::GetStats() {
    return s_UI.RenderStats;
}

void UIRenderer::ResetStats() {
    s_UI.RenderStats = {};
}

} // namespace VE
