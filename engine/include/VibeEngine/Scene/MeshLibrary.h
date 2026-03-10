/*
 * MeshLibrary — Provides built-in primitive meshes.
 *
 * Currently supports triangle and quad primitives.
 * Each mesh is created once and cached for reuse.
 */
#pragma once

#include "VibeEngine/Renderer/VertexArray.h"
#include "VibeEngine/Renderer/Shader.h"
#include <memory>

namespace VE {

class MeshLibrary {
public:
    static void Init();
    static void Shutdown();

    static std::shared_ptr<VertexArray> GetTriangle();
    static std::shared_ptr<VertexArray> GetQuad();
    static std::shared_ptr<Shader>      GetDefaultShader();

    static const char* GetMeshName(int index);
    static std::shared_ptr<VertexArray> GetMeshByIndex(int index);
    static int GetMeshCount();

private:
    static std::shared_ptr<VertexArray> s_Triangle;
    static std::shared_ptr<VertexArray> s_Quad;
    static std::shared_ptr<Shader>      s_DefaultShader;
};

} // namespace VE
