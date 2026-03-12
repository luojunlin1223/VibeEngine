#pragma once

#include "VibeEngine/Renderer/VertexArray.h"
#include <string>
#include <vector>
#include <memory>

namespace VE {

struct MeshAsset {
    std::string Name;
    std::string SourcePath; // relative to Assets/

    // CPU data: interleaved pos(3)+normal(3)+color(3)+uv(2) = 11 floats per vertex
    std::vector<float>    Vertices;
    std::vector<uint32_t> Indices;

    // GPU data
    std::shared_ptr<VertexArray> VAO;

    void Upload();  // create GPU buffers from CPU data
    void Release(); // destroy GPU buffers (keeps CPU data)
};

} // namespace VE
