/*
 * GridRenderer — GL_LINES grid with depth testing.
 *
 * Multi-layer grid (1m, 10m, 100m) that follows the camera.
 * Lines fade at layer edges. Rendered with a simple color shader
 * so depth test naturally occludes lines behind objects.
 */
#include "VibeEngine/Renderer/GridRenderer.h"
#include "VibeEngine/Renderer/RenderCommand.h"
#include "VibeEngine/Renderer/Shader.h"
#include "VibeEngine/Renderer/Buffer.h"
#include "VibeEngine/Renderer/VertexArray.h"
#include "VibeEngine/Core/Log.h"

#include <glad/gl.h>
#include <vector>
#include <cmath>
#include <algorithm>

namespace VE {

static std::shared_ptr<Shader> s_GridShader;
static std::shared_ptr<VertexArray> s_GridVAO;
static std::shared_ptr<VertexBuffer> s_GridVBO;
static constexpr int MAX_GRID_VERTICES = 4000; // enough for 3 layers

void GridRenderer::Init() {
    // Simple color-only line shader
    const char* vertSrc = R"(
        #version 460 core
        layout(location = 0) in vec3 a_Position;
        layout(location = 1) in vec4 a_Color;
        uniform mat4 u_ViewProjection;
        out vec4 v_Color;
        void main() {
            gl_Position = u_ViewProjection * vec4(a_Position, 1.0);
            v_Color = a_Color;
        }
    )";
    const char* fragSrc = R"(
        #version 460 core
        in vec4 v_Color;
        out vec4 FragColor;
        void main() {
            FragColor = v_Color;
        }
    )";
    s_GridShader = Shader::Create(vertSrc, fragSrc);

    s_GridVAO = VertexArray::Create();
    s_GridVBO = VertexBuffer::Create(nullptr, MAX_GRID_VERTICES * 7 * sizeof(float));
    s_GridVBO->SetLayout({
        { ShaderDataType::Float3, "a_Position" },
        { ShaderDataType::Float4, "a_Color" },
    });
    s_GridVAO->AddVertexBuffer(s_GridVBO);
}

void GridRenderer::Shutdown() {
    s_GridVAO.reset();
    s_GridVBO.reset();
    s_GridShader.reset();
}

void GridRenderer::DrawGrid(const glm::mat4& viewProjection,
                              const glm::vec3& cameraPos,
                              float baseSpacing, bool is2D) {
    if (!s_GridShader || !s_GridVAO) return;

    // Build line vertex data: each vertex = pos(3) + color(4) = 7 floats
    std::vector<float> vertices;
    vertices.reserve(MAX_GRID_VERTICES * 7);

    auto addLine = [&](const glm::vec3& a, const glm::vec3& b,
                        float r, float g, float bv, float alpha) {
        if (vertices.size() / 7 + 2 > MAX_GRID_VERTICES) return;
        vertices.push_back(a.x); vertices.push_back(a.y); vertices.push_back(a.z);
        vertices.push_back(r); vertices.push_back(g); vertices.push_back(bv); vertices.push_back(alpha);
        vertices.push_back(b.x); vertices.push_back(b.y); vertices.push_back(b.z);
        vertices.push_back(r); vertices.push_back(g); vertices.push_back(bv); vertices.push_back(alpha);
    };

    const float kExtent = 50000.0f;
    const float spacings[] = { baseSpacing, baseSpacing * 10.0f, baseSpacing * 100.0f };
    const int   maxLines[] = { 60, 30, 15 };
    const float baseAlpha[] = { 0.18f, 0.25f, 0.35f };

    for (int layer = 0; layer < 3; layer++) {
        float sp = spacings[layer];
        int count = maxLines[layer];
        float alpha = baseAlpha[layer];

        if (is2D) {
            float cx = std::floor(cameraPos.x / sp) * sp;
            float cy = std::floor(cameraPos.y / sp) * sp;
            for (int i = -count; i <= count; i++) {
                float gx = cx + i * sp;
                float gy = cy + i * sp;
                float fx = 1.0f - std::abs((float)i) / (float)count;
                fx = fx * fx;
                float a = fx * alpha;
                if (a < 0.01f) continue;

                bool originX = std::abs(gx) < sp * 0.01f;
                bool originY = std::abs(gy) < sp * 0.01f;

                if (originX)
                    addLine({gx,-kExtent,0}, {gx,kExtent,0}, 0.16f, 0.5f, 0.16f, 0.7f);
                else
                    addLine({gx,-kExtent,0}, {gx,kExtent,0}, 0.31f, 0.31f, 0.31f, a);

                if (originY)
                    addLine({-kExtent,gy,0}, {kExtent,gy,0}, 0.5f, 0.16f, 0.16f, 0.7f);
                else
                    addLine({-kExtent,gy,0}, {kExtent,gy,0}, 0.31f, 0.31f, 0.31f, a);
            }
        } else {
            float cx = std::floor(cameraPos.x / sp) * sp;
            float cz = std::floor(cameraPos.z / sp) * sp;
            for (int i = -count; i <= count; i++) {
                float gx = cx + i * sp;
                float gz = cz + i * sp;
                float fx = 1.0f - std::abs((float)i) / (float)count;
                fx = fx * fx;
                float a = fx * alpha;
                if (a < 0.01f) continue;

                bool originX = std::abs(gx) < sp * 0.01f;
                bool originZ = std::abs(gz) < sp * 0.01f;

                // Lines parallel to Z axis
                if (originX)
                    addLine({gx,0,-kExtent}, {gx,0,kExtent}, 0.16f, 0.16f, 0.5f, 0.7f);
                else
                    addLine({gx,0,-kExtent}, {gx,0,kExtent}, 0.31f, 0.31f, 0.31f, a);

                // Lines parallel to X axis
                if (originZ)
                    addLine({-kExtent,0,gz}, {kExtent,0,gz}, 0.5f, 0.16f, 0.16f, 0.7f);
                else
                    addLine({-kExtent,0,gz}, {kExtent,0,gz}, 0.31f, 0.31f, 0.31f, a);
            }
        }
    }

    if (vertices.empty()) return;

    uint32_t vertCount = static_cast<uint32_t>(vertices.size() / 7);

    // Upload and draw
    s_GridVBO->SetData(vertices.data(), static_cast<uint32_t>(vertices.size() * sizeof(float)));

    // GL state: depth read (no write), alpha blend
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    s_GridShader->Bind();
    s_GridShader->SetMat4("u_ViewProjection", viewProjection);

    s_GridVAO->Bind();
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(vertCount));

    // Restore
    glDepthMask(GL_TRUE);
}

} // namespace VE
