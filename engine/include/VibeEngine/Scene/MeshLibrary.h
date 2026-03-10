/*
 * MeshLibrary — Provides built-in primitive meshes.
 *
 * Supports 2D primitives (Triangle, Quad) with flat color shader,
 * and 3D primitives (Cube) with lit shader.
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
    static std::shared_ptr<VertexArray> GetCube();
    static std::shared_ptr<Shader>      GetDefaultShader();  // unlit (2D)
    static std::shared_ptr<Shader>      GetLitShader();      // lit (3D)

    static const char* GetMeshName(int index);
    static std::shared_ptr<VertexArray> GetMeshByIndex(int index);
    static bool IsLitMesh(int index);
    static int GetMeshCount();

private:
    static std::shared_ptr<VertexArray> s_Triangle;
    static std::shared_ptr<VertexArray> s_Quad;
    static std::shared_ptr<VertexArray> s_Cube;
    static std::shared_ptr<Shader>      s_DefaultShader;
    static std::shared_ptr<Shader>      s_LitShader;
};

} // namespace VE
