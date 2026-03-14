/*
 * LODSystem — LOD mesh generation utilities.
 */
#include "VibeEngine/Renderer/LODSystem.h"
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace VE {

std::shared_ptr<MeshAsset> LODMeshGenerator::CreateSphere(int rings, int segments) {
    auto mesh = std::make_shared<MeshAsset>();

    // Generate vertices: pos(3) + normal(3) + color(3) + uv(2) = 11 floats
    for (int r = 0; r <= rings; ++r) {
        float phi = static_cast<float>(M_PI) * r / rings;
        float y = std::cos(phi);
        float sinPhi = std::sin(phi);
        float v = static_cast<float>(r) / rings;

        for (int s = 0; s <= segments; ++s) {
            float theta = 2.0f * static_cast<float>(M_PI) * s / segments;
            float x = sinPhi * std::cos(theta);
            float z = sinPhi * std::sin(theta);
            float u = static_cast<float>(s) / segments;

            // Position
            mesh->Vertices.push_back(x);
            mesh->Vertices.push_back(y);
            mesh->Vertices.push_back(z);
            // Normal (same as position for unit sphere)
            mesh->Vertices.push_back(x);
            mesh->Vertices.push_back(y);
            mesh->Vertices.push_back(z);
            // Color (white)
            mesh->Vertices.push_back(1.0f);
            mesh->Vertices.push_back(1.0f);
            mesh->Vertices.push_back(1.0f);
            // UV
            mesh->Vertices.push_back(u);
            mesh->Vertices.push_back(v);
        }
    }

    // Generate indices
    for (int r = 0; r < rings; ++r) {
        for (int s = 0; s < segments; ++s) {
            uint32_t a = r * (segments + 1) + s;
            uint32_t b = a + (segments + 1);
            mesh->Indices.push_back(a);
            mesh->Indices.push_back(b);
            mesh->Indices.push_back(a + 1);
            mesh->Indices.push_back(b);
            mesh->Indices.push_back(b + 1);
            mesh->Indices.push_back(a + 1);
        }
    }

    mesh->ComputeBoundingBox();
    mesh->Upload();
    return mesh;
}

std::shared_ptr<MeshAsset> LODMeshGenerator::CreateCube() {
    auto mesh = std::make_shared<MeshAsset>();

    // 6 faces, 4 vertices each = 24 vertices
    // Each vertex: pos(3) + normal(3) + color(3) + uv(2) = 11 floats
    struct FaceData { float nx, ny, nz; float verts[4][3]; float uvs[4][2]; };
    FaceData faces[6] = {
        // Front (+Z)
        { 0, 0, 1, {{-0.5f,-0.5f,0.5f},{0.5f,-0.5f,0.5f},{0.5f,0.5f,0.5f},{-0.5f,0.5f,0.5f}},
          {{0,0},{1,0},{1,1},{0,1}} },
        // Back (-Z)
        { 0, 0,-1, {{0.5f,-0.5f,-0.5f},{-0.5f,-0.5f,-0.5f},{-0.5f,0.5f,-0.5f},{0.5f,0.5f,-0.5f}},
          {{0,0},{1,0},{1,1},{0,1}} },
        // Right (+X)
        { 1, 0, 0, {{0.5f,-0.5f,0.5f},{0.5f,-0.5f,-0.5f},{0.5f,0.5f,-0.5f},{0.5f,0.5f,0.5f}},
          {{0,0},{1,0},{1,1},{0,1}} },
        // Left (-X)
        {-1, 0, 0, {{-0.5f,-0.5f,-0.5f},{-0.5f,-0.5f,0.5f},{-0.5f,0.5f,0.5f},{-0.5f,0.5f,-0.5f}},
          {{0,0},{1,0},{1,1},{0,1}} },
        // Top (+Y)
        { 0, 1, 0, {{-0.5f,0.5f,0.5f},{0.5f,0.5f,0.5f},{0.5f,0.5f,-0.5f},{-0.5f,0.5f,-0.5f}},
          {{0,0},{1,0},{1,1},{0,1}} },
        // Bottom (-Y)
        { 0,-1, 0, {{-0.5f,-0.5f,-0.5f},{0.5f,-0.5f,-0.5f},{0.5f,-0.5f,0.5f},{-0.5f,-0.5f,0.5f}},
          {{0,0},{1,0},{1,1},{0,1}} },
    };

    for (int f = 0; f < 6; ++f) {
        uint32_t base = static_cast<uint32_t>(mesh->Vertices.size() / 11);
        for (int v = 0; v < 4; ++v) {
            mesh->Vertices.push_back(faces[f].verts[v][0]);
            mesh->Vertices.push_back(faces[f].verts[v][1]);
            mesh->Vertices.push_back(faces[f].verts[v][2]);
            mesh->Vertices.push_back(faces[f].nx);
            mesh->Vertices.push_back(faces[f].ny);
            mesh->Vertices.push_back(faces[f].nz);
            mesh->Vertices.push_back(1.0f); mesh->Vertices.push_back(1.0f); mesh->Vertices.push_back(1.0f);
            mesh->Vertices.push_back(faces[f].uvs[v][0]);
            mesh->Vertices.push_back(faces[f].uvs[v][1]);
        }
        mesh->Indices.push_back(base);
        mesh->Indices.push_back(base + 1);
        mesh->Indices.push_back(base + 2);
        mesh->Indices.push_back(base);
        mesh->Indices.push_back(base + 2);
        mesh->Indices.push_back(base + 3);
    }

    mesh->ComputeBoundingBox();
    mesh->Upload();
    return mesh;
}

} // namespace VE
