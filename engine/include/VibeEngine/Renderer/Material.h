/*
 * Material — Combines a Shader with per-material property overrides.
 *
 * A Material references a shader and stores property values (colors, floats,
 * textures) that are uploaded as uniforms when the material is bound.
 * Materials can be saved/loaded as .vmat YAML files.
 */
#pragma once

#include "VibeEngine/Renderer/Shader.h"
#include "VibeEngine/Renderer/Texture.h"
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <algorithm>

namespace VE {

enum class MaterialPropertyType { Float, Int, Vec3, Vec4, Texture2D };

struct MaterialProperty {
    std::string Name;
    std::string DisplayName; // from ShaderLab (e.g. "Roughness")
    MaterialPropertyType Type;
    // Values
    float       FloatValue = 0.0f;
    int         IntValue   = 0;
    glm::vec3   Vec3Value  = glm::vec3(0.0f);
    glm::vec4   Vec4Value  = glm::vec4(1.0f);
    std::string TexturePath;
    std::shared_ptr<Texture2D> TextureRef;
    // Range metadata (for Range properties in ShaderLab)
    bool  IsRange  = false;
    float RangeMin = 0.0f;
    float RangeMax = 1.0f;
};

class Material {
public:
    Material(const std::string& name, const std::shared_ptr<Shader>& shader);

    static std::shared_ptr<Material> Create(const std::string& name,
                                             const std::shared_ptr<Shader>& shader);
    static std::shared_ptr<Material> Load(const std::string& filePath);
    void Save(const std::string& filePath) const;

    void SetShader(const std::shared_ptr<Shader>& shader) { m_Shader = shader; }
    std::shared_ptr<Shader> GetShader() const { return m_Shader; }

    const std::string& GetName() const { return m_Name; }
    void SetName(const std::string& name) { m_Name = name; }
    const std::string& GetFilePath() const { return m_FilePath; }

    // Bind shader and upload all material properties as uniforms
    void Bind() const;

    /// Auto-populate properties from the shader's ShaderLab property declarations.
    /// Only adds properties that don't already exist.
    void PopulateFromShader();

    // Property accessors
    void SetFloat(const std::string& name, float value);
    void SetInt(const std::string& name, int value);
    void SetVec3(const std::string& name, const glm::vec3& value);
    void SetVec4(const std::string& name, const glm::vec4& value);
    void SetTexture(const std::string& name, const std::string& path);

    std::vector<MaterialProperty>& GetProperties() { return m_Properties; }
    const std::vector<MaterialProperty>& GetProperties() const { return m_Properties; }

    // Whether this material's shader uses lighting (heuristic: has u_LightDir property)
    bool IsLit() const { return m_IsLit; }
    void SetLit(bool lit) { m_IsLit = lit; }

    // Whether this material is transparent (shader has blend enabled)
    bool IsTransparent() const { return m_Shader && m_Shader->IsTransparent(); }

private:
    MaterialProperty* FindProperty(const std::string& name);

    std::string m_Name;
    std::string m_FilePath;
    std::shared_ptr<Shader> m_Shader;
    std::vector<MaterialProperty> m_Properties;
    bool m_IsLit = false;
};

// Global material registry
class MaterialLibrary {
public:
    static void Init();
    static void Shutdown();
    static void Register(const std::shared_ptr<Material>& material);
    static std::shared_ptr<Material> Get(const std::string& name);
    static std::vector<std::string> GetAllNames();

private:
    static std::unordered_map<std::string, std::shared_ptr<Material>> s_Materials;
};

} // namespace VE
