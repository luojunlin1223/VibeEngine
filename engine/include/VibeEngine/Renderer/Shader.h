#pragma once

#include "VibeEngine/Renderer/ShaderLab.h"
#include <string>
#include <memory>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <glm/glm.hpp>

namespace VE {

/// Metadata for a shader property declared in ShaderLab's Properties {} block.
/// Stored on the Shader so Material can auto-populate its defaults.
enum class ShaderPropertyType { Float, Range, Int, Color, Vector, Texture2D };

struct ShaderPropertyInfo {
    std::string Name;          // uniform name (e.g. "_Metallic" → "u_Metallic")
    std::string DisplayName;   // human-readable (e.g. "Metallic")
    ShaderPropertyType Type = ShaderPropertyType::Float;
    float       FloatDefault = 0.0f;
    int         IntDefault   = 0;
    glm::vec4   VectorDefault = glm::vec4(0.0f);
    std::string TextureDefault;  // "white", "black", etc.
    float RangeMin = 0.0f;
    float RangeMax = 1.0f;
};

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

    /// Property metadata from ShaderLab Properties {} block
    const std::vector<ShaderPropertyInfo>& GetPropertyInfos() const { return m_PropertyInfos; }
    void SetPropertyInfos(std::vector<ShaderPropertyInfo> infos) { m_PropertyInfos = std::move(infos); }

    /// Render state from ShaderLab Pass (blend, zwrite, cull, etc.)
    const ShaderLabRenderState& GetRenderState() const { return m_RenderState; }
    void SetRenderState(const ShaderLabRenderState& rs) { m_RenderState = rs; }
    bool IsTransparent() const { return m_RenderState.BlendEnabled; }

    /// Apply this shader's render state to the GL pipeline
    void ApplyRenderState() const;

    static std::shared_ptr<Shader> Create(const std::string& vertexSrc, const std::string& fragmentSrc);

    /// Load and compile a .shader (ShaderLab) file from disk.
    static std::shared_ptr<Shader> CreateFromFile(const std::string& filePath);

protected:
    std::string m_Name;
    std::vector<ShaderPropertyInfo> m_PropertyInfos;
    ShaderLabRenderState m_RenderState;
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
