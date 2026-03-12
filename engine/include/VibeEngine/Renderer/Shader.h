#pragma once

#include <string>
#include <memory>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <glm/glm.hpp>

namespace VE {

class Shader {
public:
    virtual ~Shader() = default;

    virtual void Bind() const = 0;
    virtual void Unbind() const = 0;

    virtual void SetMat4(const std::string& name, const glm::mat4& value) = 0;
    virtual void SetVec4(const std::string& name, const glm::vec4& value) = 0;
    virtual void SetVec3(const std::string& name, const glm::vec3& value) = 0;
    virtual void SetFloat(const std::string& name, float value) = 0;
    virtual void SetInt(const std::string& name, int value) = 0;

    const std::string& GetName() const { return m_Name; }
    void SetName(const std::string& name) { m_Name = name; }

    static std::shared_ptr<Shader> Create(const std::string& vertexSrc, const std::string& fragmentSrc);

    /// Load and compile a .shader (ShaderLab) file from disk.
    static std::shared_ptr<Shader> CreateFromFile(const std::string& filePath);

protected:
    std::string m_Name;
};

/// Global shader registry — tracks all loaded shaders by name.
class ShaderLibrary {
public:
    static void Register(const std::string& name, const std::shared_ptr<Shader>& shader);
    static void Remove(const std::string& name);
    static std::shared_ptr<Shader> Get(const std::string& name);
    static bool Exists(const std::string& name);
    static std::vector<std::string> GetAllNames();
    static void Shutdown();

private:
    static std::unordered_map<std::string, std::shared_ptr<Shader>> s_Shaders;
};

} // namespace VE
