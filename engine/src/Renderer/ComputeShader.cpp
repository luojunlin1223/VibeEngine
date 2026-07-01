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

#include <algorithm>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <filesystem>

namespace VE {

namespace {

std::string PreprocessComputeIncludes(const std::string& source,
                                      const std::string& baseDir,
                                      std::set<std::string>& alreadyIncluded) {
    static const std::regex includeRegex(R"(^\s*#include\s+\"([^\"]+)\"\s*$)");

    std::istringstream stream(source);
    std::string line;
    std::string result;
    result.reserve(source.size());

    while (std::getline(stream, line)) {
        std::smatch match;
        if (!std::regex_match(line, match, includeRegex)) {
            result += line + "\n";
            continue;
        }

        const std::string filename = match[1].str();
        std::filesystem::path includePath = std::filesystem::path(baseDir) / filename;
        std::string fullPath = includePath.lexically_normal().string();
        std::replace(fullPath.begin(), fullPath.end(), '\\', '/');

        if (alreadyIncluded.count(fullPath)) {
            result += "// [ComputeShader] skipped duplicate #include \"" + filename + "\"\n";
            continue;
        }

        std::ifstream file(fullPath, std::ios::in | std::ios::binary);
        if (!file) {
            VE_ENGINE_WARN("ComputeShader: #include \"{}\" not found (searched: {})", filename, fullPath);
            result += "// [ComputeShader] ERROR: #include \"" + filename + "\" not found\n";
            continue;
        }

        std::ostringstream ss;
        ss << file.rdbuf();
        alreadyIncluded.insert(fullPath);

        std::string processed = PreprocessComputeIncludes(ss.str(), baseDir, alreadyIncluded);
        result += "// [ComputeShader] begin #include \"" + filename + "\"\n";
        result += processed;
        if (!processed.empty() && processed.back() != '\n')
            result += "\n";
        result += "// [ComputeShader] end #include \"" + filename + "\"\n";
    }

    return result;
}

}

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

    std::string baseDir = "shaders";
    std::filesystem::path shaderPath(filePath);
    if (shaderPath.has_parent_path())
        baseDir = shaderPath.parent_path().string();
    std::replace(baseDir.begin(), baseDir.end(), '\\', '/');

    std::set<std::string> alreadyIncluded;
    std::string source = PreprocessComputeIncludes(ss.str(), baseDir, alreadyIncluded);
    auto shader = ComputeShader::Create(source);
    if (shader)
        shader->SetName(filePath);
    return shader;
}

} // namespace VE
