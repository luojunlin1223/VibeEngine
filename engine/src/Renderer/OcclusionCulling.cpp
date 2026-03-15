/*
 * OcclusionCulling -- GPU occlusion queries implementation.
 *
 * Approach: GL_ANY_SAMPLES_PASSED occlusion queries with temporal (previous-
 * frame) result reuse to avoid GPU stalls. Each visible entity's world-space
 * AABB is rendered as a depth-only box; the query result determines next
 * frame's visibility.
 */
#include "VibeEngine/Renderer/OcclusionCulling.h"
#include "VibeEngine/Renderer/Shader.h"
#include "VibeEngine/Renderer/Buffer.h"
#include "VibeEngine/Renderer/VertexArray.h"
#include "VibeEngine/Core/Log.h"

#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>

namespace VE {

// ── Internal state ──────────────────────────────────────────────────────

struct QueryInfo {
    GLuint QueryID = 0;   // GL query object
    bool   Pending = false; // query was issued this frame
};

// Double-buffered: current frame writes to s_WriteBuffer, reads from s_ReadBuffer.
static std::unordered_map<uint32_t, QueryInfo> s_Buffers[2];
static int  s_WriteIdx = 0;  // index into s_Buffers for current-frame writes
static int  s_ReadIdx  = 1;  // index into s_Buffers for previous-frame reads
static bool s_Initialized = false;

// Cached visibility from previous frame: true = visible, false = occluded.
static std::unordered_map<uint32_t, bool> s_Visibility;

// Per-frame occluded count
static uint32_t s_OccludedCount = 0;

// Depth-only shader and unit cube VAO for bounding box rendering
static std::shared_ptr<Shader>      s_DepthShader;
static std::shared_ptr<VertexArray>  s_BoxVAO;

// ── Helpers ─────────────────────────────────────────────────────────────

void OcclusionCulling::CreateBoxVAO() {
    // Unit cube vertices (position only), centered at origin, size 1x1x1
    float vertices[] = {
        // Front face
        -0.5f, -0.5f,  0.5f,
         0.5f, -0.5f,  0.5f,
         0.5f,  0.5f,  0.5f,
        -0.5f,  0.5f,  0.5f,
        // Back face
        -0.5f, -0.5f, -0.5f,
         0.5f, -0.5f, -0.5f,
         0.5f,  0.5f, -0.5f,
        -0.5f,  0.5f, -0.5f,
    };

    uint32_t indices[] = {
        // Front
        0, 1, 2, 2, 3, 0,
        // Right
        1, 5, 6, 6, 2, 1,
        // Back
        5, 4, 7, 7, 6, 5,
        // Left
        4, 0, 3, 3, 7, 4,
        // Top
        3, 2, 6, 6, 7, 3,
        // Bottom
        4, 5, 1, 1, 0, 4,
    };

    s_BoxVAO = VertexArray::Create();

    auto vbo = VertexBuffer::Create(vertices, sizeof(vertices));
    vbo->SetLayout({
        { ShaderDataType::Float3, "a_Position" }
    });
    s_BoxVAO->AddVertexBuffer(vbo);

    auto ibo = IndexBuffer::Create(indices, 36);
    s_BoxVAO->SetIndexBuffer(ibo);
}

void OcclusionCulling::CreateDepthOnlyShader() {
    // Minimal shader: transforms position, writes depth only (no color output)
    const std::string vertSrc = R"(
#version 460 core
layout(location = 0) in vec3 a_Position;
uniform mat4 u_MVP;
void main() {
    gl_Position = u_MVP * vec4(a_Position, 1.0);
}
)";

    const std::string fragSrc = R"(
#version 460 core
void main() {
    // No color output — depth write only
}
)";

    s_DepthShader = Shader::Create(vertSrc, fragSrc);
    if (s_DepthShader)
        s_DepthShader->SetName("__OcclusionDepthOnly");
}

// ── Public API ──────────────────────────────────────────────────────────

void OcclusionCulling::Init() {
    if (s_Initialized) return;

    CreateBoxVAO();
    CreateDepthOnlyShader();

    s_Buffers[0].clear();
    s_Buffers[1].clear();
    s_Visibility.clear();
    s_WriteIdx = 0;
    s_ReadIdx  = 1;
    s_Initialized = true;

    VE_ENGINE_INFO("OcclusionCulling initialized (GL hardware queries)");
}

void OcclusionCulling::Shutdown() {
    if (!s_Initialized) return;

    // Delete all GL query objects
    for (int b = 0; b < 2; ++b) {
        for (auto& [id, qi] : s_Buffers[b]) {
            if (qi.QueryID)
                glDeleteQueries(1, &qi.QueryID);
        }
        s_Buffers[b].clear();
    }

    s_Visibility.clear();
    s_BoxVAO.reset();
    s_DepthShader.reset();
    s_Initialized = false;
}

void OcclusionCulling::BeginFrame() {
    if (!s_Initialized) return;

    s_OccludedCount = 0;

    // Swap buffers: what was "write" is now "read" (previous frame's queries)
    std::swap(s_WriteIdx, s_ReadIdx);

    // Collect results from the now-read buffer (previous frame's queries)
    auto& readBuf = s_Buffers[s_ReadIdx];
    for (auto& [entityID, qi] : readBuf) {
        if (!qi.Pending || qi.QueryID == 0) continue;

        GLuint result = 0;
        // GL_QUERY_RESULT will block until the result is available, but since
        // these are from the *previous* frame the GPU should have finished them.
        glGetQueryObjectuiv(qi.QueryID, GL_QUERY_RESULT, &result);

        s_Visibility[entityID] = (result != 0);
        qi.Pending = false;
    }

    // Clear write buffer's pending flags for this frame
    for (auto& [id, qi] : s_Buffers[s_WriteIdx]) {
        qi.Pending = false;
    }
}

void OcclusionCulling::EndFrame() {
    // Nothing needed for now — results are collected in next BeginFrame
}

bool OcclusionCulling::IsOccluded(uint32_t entityID) {
    auto it = s_Visibility.find(entityID);
    if (it == s_Visibility.end())
        return false; // not tracked yet — assume visible

    if (!it->second) {
        s_OccludedCount++;
        return true;
    }
    return false;
}

void OcclusionCulling::QueryEntity(uint32_t entityID,
                                    const glm::vec3& aabbMin,
                                    const glm::vec3& aabbMax,
                                    const glm::mat4& viewProjection) {
    if (!s_Initialized || !s_DepthShader || !s_BoxVAO) return;

    auto& writeBuf = s_Buffers[s_WriteIdx];

    // Ensure a GL query object exists for this entity in the write buffer
    auto& qi = writeBuf[entityID];
    if (qi.QueryID == 0) {
        glGenQueries(1, &qi.QueryID);
    }

    // Compute model matrix that transforms the unit cube [-0.5, 0.5]^3
    // to cover the world-space AABB [aabbMin, aabbMax]
    glm::vec3 center = (aabbMin + aabbMax) * 0.5f;
    glm::vec3 size   = aabbMax - aabbMin;
    glm::mat4 model  = glm::translate(glm::mat4(1.0f), center)
                      * glm::scale(glm::mat4(1.0f), size);
    glm::mat4 mvp    = viewProjection * model;

    // Save GL state
    GLboolean prevColorMask[4];
    glGetBooleanv(GL_COLOR_WRITEMASK, prevColorMask);
    GLboolean prevDepthMask;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);

    // Disable color writes, disable depth writes (we only test against existing depth)
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDepthMask(GL_FALSE);

    // Bind depth-only shader and box VAO
    s_DepthShader->Bind();
    s_DepthShader->SetMat4("u_MVP", mvp);

    // Issue occlusion query
    glBeginQuery(GL_ANY_SAMPLES_PASSED, qi.QueryID);
    s_BoxVAO->Bind();
    glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_INT, nullptr);
    s_BoxVAO->Unbind();
    glEndQuery(GL_ANY_SAMPLES_PASSED);

    qi.Pending = true;

    // Restore GL state
    glColorMask(prevColorMask[0], prevColorMask[1], prevColorMask[2], prevColorMask[3]);
    glDepthMask(prevDepthMask);
}

void OcclusionCulling::RemoveEntity(uint32_t entityID) {
    s_Visibility.erase(entityID);
    for (int b = 0; b < 2; ++b) {
        auto it = s_Buffers[b].find(entityID);
        if (it != s_Buffers[b].end()) {
            if (it->second.QueryID)
                glDeleteQueries(1, &it->second.QueryID);
            s_Buffers[b].erase(it);
        }
    }
}

void OcclusionCulling::Reset() {
    for (int b = 0; b < 2; ++b) {
        for (auto& [id, qi] : s_Buffers[b]) {
            if (qi.QueryID)
                glDeleteQueries(1, &qi.QueryID);
        }
        s_Buffers[b].clear();
    }
    s_Visibility.clear();
    s_OccludedCount = 0;
}

uint32_t OcclusionCulling::GetOccludedCount() {
    return s_OccludedCount;
}

} // namespace VE
