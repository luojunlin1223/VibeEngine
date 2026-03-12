#pragma once

#include "VibeEngine/Core/Object.h"
#include "VibeEngine/Renderer/VertexArray.h"
#include <string>
#include <vector>
#include <memory>

namespace VE {

struct MeshAsset : public Object {
    std::string SourcePath; // absolute path to source file

    // CPU data: interleaved pos(3)+normal(3)+color(3)+uv(2) = 11 floats per vertex
    std::vector<float>    Vertices;
    std::vector<uint32_t> Indices;

    // GPU data
    std::shared_ptr<VertexArray> VAO;

    void Upload();  // create GPU buffers from CPU data
    void Release(); // destroy GPU buffers (keeps CPU data)
};

} // namespace VE
