#include "VibeEngine/Asset/MeshAsset.h"
#include "VibeEngine/Renderer/Buffer.h"
#include "VibeEngine/Core/Log.h"

namespace VE {

void MeshAsset::Upload() {
    if (Vertices.empty() || Indices.empty()) return;

    VAO = VertexArray::Create();
    auto vb = VertexBuffer::Create(Vertices.data(),
        static_cast<uint32_t>(Vertices.size() * sizeof(float)));
    vb->SetLayout({
        { ShaderDataType::Float3, "a_Position" },
        { ShaderDataType::Float3, "a_Normal"   },
        { ShaderDataType::Float3, "a_Color"    },
        { ShaderDataType::Float2, "a_TexCoord" },
    });
    VAO->AddVertexBuffer(vb);
    VAO->SetIndexBuffer(IndexBuffer::Create(Indices.data(),
        static_cast<uint32_t>(Indices.size())));

    VE_ENGINE_INFO("MeshAsset uploaded: {0} ({1} verts, {2} tris)",
        GetName(), Vertices.size() / 11, Indices.size() / 3);
}

void MeshAsset::Release() {
    VAO.reset();
}

} // namespace VE
