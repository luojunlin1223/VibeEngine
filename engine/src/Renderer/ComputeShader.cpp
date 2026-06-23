/*
 * ComputeShader is the minimal backend abstraction needed to port HPWater's
 * compute-driven passes without forcing ShaderLab to understand non-raster
 * kernels yet. The first consumer is HPWater caustic irradiance; broader
 * resource binding and reflection can be added once more compute passes move
 * over from the current fullscreen fallback path.
 */
#include "VibeEngine/Renderer/ComputeShader.h"
#include "VibeEngine/Renderer/RendererAPI.h"
#include "VibeEngine/Platform/OpenGL/OpenGLComputeShader.h"
#include "VibeEngine/Core/Log.h"

#include <fstream>
#include <sstream>

namespace VE {

std::shared_ptr<ComputeShader> ComputeShader::Create(const std::string& computeSrc) {
    switch (RendererAPI::GetAPI()) {
        case RendererAPI::API::OpenGL:
            return std::make_shared<OpenGLComputeShader>(computeSrc);
        case RendererAPI::API::Vulkan:
            VE_ENGINE_ERROR("ComputeShader::Create - Vulkan backend is not implemented");
            return nullptr;
        default:
            VE_ENGINE_ERROR("ComputeShader::Create - unsupported API");
            return nullptr;
    }
}

std::shared_ptr<ComputeShader> ComputeShader::CreateFromFile(const std::string& filePath) {
    std::ifstream file(filePath, std::ios::in | std::ios::binary);
    if (!file) {
        VE_ENGINE_ERROR("ComputeShader::CreateFromFile('{}') failed to open file", filePath);
        return nullptr;
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    auto shader = ComputeShader::Create(ss.str());
    if (shader)
        shader->SetName(filePath);
    return shader;
}

} // namespace VE
