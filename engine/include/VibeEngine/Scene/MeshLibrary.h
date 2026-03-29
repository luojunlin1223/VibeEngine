/*
 * MeshLibrary — Provides built-in primitive meshes.
 *
 * Supports 2D primitives (Triangle, Quad) with flat color shader,
 * and 3D primitives (Cube) with lit shader.
 */
#pragma once

#include "VibeEngine/Renderer/VertexArray.h"
#include "VibeEngine/Renderer/Shader.h"
#include "VibeEngine/Renderer/Material.h"
#include "VibeEngine/Asset/MeshAsset.h"
#include <memory>

namespace VE {

class MeshLibrary {
public:
    static void Init();
    static void Shutdown();

    static std::shared_ptr<VertexArray> GetTriangle();
    static std::shared_ptr<VertexArray> GetQuad();
    static std::shared_ptr<VertexArray> GetCube();
    static std::shared_ptr<VertexArray> GetSphere();
    static std::shared_ptr<VertexArray> GetPlane();
    static std::shared_ptr<VertexArray> GetSkySphere();      // inside-facing for sky rendering
    static std::shared_ptr<Shader>      GetDefaultShader();  // unlit (2D)
    static std::shared_ptr<Shader>      GetLitShader();      // lit (3D)
    static std::shared_ptr<Shader>      GetSkyShader();      // sky gradient/texture

    static const char* GetMeshName(int index);
    static std::shared_ptr<VertexArray> GetMeshByIndex(int index);
    static bool IsLitMesh(int index);
    static int GetMeshCount();
    static AABB GetMeshAABB(int index);

private:
    static std::shared_ptr<VertexArray> s_Triangle;
    static std::shared_ptr<VertexArray> s_Quad;
    static std::shared_ptr<VertexArray> s_Cube;
    static std::shared_ptr<VertexArray> s_Sphere;
    static std::shared_ptr<VertexArray> s_Plane;
    static std::shared_ptr<VertexArray> s_SkySphere;
    static std::shared_ptr<Shader>      s_DefaultShader;
    static std::shared_ptr<Shader>      s_LitShader;
    static std::shared_ptr<Shader>      s_SkyShader;
};

} // namespace VE
