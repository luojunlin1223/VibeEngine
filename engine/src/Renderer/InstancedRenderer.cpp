/*
 * InstancedRenderer — GPU-instanced mesh rendering implementation.
 *
 * Design:
 *   1. Submit() collects instances grouped by (mesh, material) key.
 *   2. EndScene() iterates each group, uploads instance data to a
 *      dynamic VBO, builds a one-off instanced VAO, and issues
 *      glDrawElementsInstanced.
 *
 * The instance VBO layout:
 *   location 4-7 : mat4  a_InstanceModel   (64 bytes, divisor=1)
 *   location 8   : vec4  a_InstanceColor   (16 bytes, divisor=1)
 *   Total stride = 80 bytes per instance.
 */

#include "VibeEngine/Renderer/InstancedRenderer.h"
#include "VibeEngine/Renderer/RenderCommand.h"
#include "VibeEngine/Renderer/Shader.h"
#include "VibeEngine/Core/Log.h"

#include <unordered_map>

namespace VE {

// ── Static state ────────────────────────────────────────────────────

static constexpr uint32_t MAX_INSTANCES = 65536;

struct BatchKey {
    VertexArray* Mesh;
    Material*    Mat;
    bool operator==(const BatchKey& o) const { return Mesh == o.Mesh && Mat == o.Mat; }
};

struct BatchKeyHash {
    size_t operator()(const BatchKey& k) const {
        size_t h1 = std::hash<void*>{}(k.Mesh);
        size_t h2 = std::hash<void*>{}(k.Mat);
        return h1 ^ (h2 << 1);
    }
};

struct Batch {
    std::shared_ptr<VertexArray>  MeshRef;
    std::shared_ptr<Material>     MatRef;
    std::vector<InstanceData>     Instances;
};

static std::unordered_map<BatchKey, Batch, BatchKeyHash> s_Batches;
static std::shared_ptr<VertexBuffer> s_InstanceVBO;
static glm::mat4 s_ViewProjection;
static InstancedRenderer::Stats s_Stats;

static std::shared_ptr<Shader> s_UnlitInstancedShader;
static std::shared_ptr<Shader> s_LitInstancedShader;

// ── Init / Shutdown ─────────────────────────────────────────────────

void InstancedRenderer::Init() {
    // Pre-allocate a large dynamic VBO for instance data
    uint32_t bufferSize = MAX_INSTANCES * sizeof(InstanceData);
    s_InstanceVBO = VertexBuffer::Create(nullptr, bufferSize);
    s_InstanceVBO->SetLayout({
        { ShaderDataType::Mat4,   "a_InstanceModel" },
        { ShaderDataType::Float4, "a_InstanceColor"  },
    });

    // Load instanced shaders
    s_UnlitInstancedShader = Shader::CreateFromFile("shaders/UnlitInstanced.shader");
    s_LitInstancedShader   = Shader::CreateFromFile("shaders/LitInstanced.shader");

    if (!s_UnlitInstancedShader)
        VE_ENGINE_ERROR("InstancedRenderer: failed to load UnlitInstanced.shader");
    if (!s_LitInstancedShader)
        VE_ENGINE_ERROR("InstancedRenderer: failed to load LitInstanced.shader");

    VE_ENGINE_INFO("InstancedRenderer initialized (max {0} instances)", MAX_INSTANCES);
}

void InstancedRenderer::Shutdown() {
    s_InstanceVBO.reset();
    s_UnlitInstancedShader.reset();
    s_LitInstancedShader.reset();
    s_Batches.clear();
}

// ── Per-frame ───────────────────────────────────────────────────────

void InstancedRenderer::BeginScene(const glm::mat4& viewProjection) {
    s_ViewProjection = viewProjection;
    s_Batches.clear();
}

void InstancedRenderer::Submit(const std::shared_ptr<VertexArray>& mesh,
                               const std::shared_ptr<Material>& material,
                               const glm::mat4& model,
                               const glm::vec4& color) {
    BatchKey key{ mesh.get(), material.get() };
    auto& batch = s_Batches[key];
    if (batch.Instances.empty()) {
        batch.MeshRef = mesh;
        batch.MatRef  = material;
    }
    batch.Instances.push_back({ model, color });
}

void InstancedRenderer::EndScene() {
    for (auto& [key, batch] : s_Batches) {
        uint32_t count = static_cast<uint32_t>(batch.Instances.size());
        if (count == 0) continue;

        // Clamp to buffer capacity
        if (count > MAX_INSTANCES) count = MAX_INSTANCES;

        // Upload instance data to the shared VBO
        s_InstanceVBO->SetData(batch.Instances.data(),
                               count * sizeof(InstanceData));

        // Build a temporary instanced VAO:
        //   - Share the original mesh's vertex + index buffers
        //   - Attach the instance VBO with divisors
        auto instancedVAO = VertexArray::Create();

        // Re-attach original vertex buffer(s)
        for (auto& vb : batch.MeshRef->GetVertexBuffers())
            instancedVAO->AddVertexBuffer(vb);

        // Attach instance buffer (sets divisor=1 for each attribute)
        instancedVAO->AddInstanceBuffer(s_InstanceVBO);

        // Share index buffer
        instancedVAO->SetIndexBuffer(batch.MeshRef->GetIndexBuffer());

        // Choose the instanced shader variant
        bool lit = batch.MatRef && batch.MatRef->IsLit();
        auto shader = lit ? s_LitInstancedShader : s_UnlitInstancedShader;
        if (!shader) continue;

        // Bind material (textures + material-level uniforms) then override shader
        batch.MatRef->Bind();
        shader->Bind();
        shader->SetMat4("u_ViewProjection", s_ViewProjection);

        // Lighting uniforms are set by Scene::OnRender before EndScene is called,
        // so we need the caller to set them on the shader. We expose the shaders
        // for that purpose via GetLitInstancedShader() / GetUnlitInstancedShader().

        RenderCommand::DrawIndexedInstanced(instancedVAO, count);

        s_Stats.DrawCalls++;
        s_Stats.InstanceCount += count;
    }
}

// ── Stats ───────────────────────────────────────────────────────────

InstancedRenderer::Stats InstancedRenderer::GetStats() { return s_Stats; }
void InstancedRenderer::ResetStats() { s_Stats = {}; }

// ── Shader access ───────────────────────────────────────────────────

std::shared_ptr<Shader> InstancedRenderer::GetUnlitInstancedShader() { return s_UnlitInstancedShader; }
std::shared_ptr<Shader> InstancedRenderer::GetLitInstancedShader()   { return s_LitInstancedShader; }

} // namespace VE
