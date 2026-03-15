#include "VibeEngine/Renderer/Material.h"
#include "VibeEngine/Core/Log.h"

#include <yaml-cpp/yaml.h>
#include <fstream>

namespace VE {

// ── Material ────────────────────────────────────────────────────────

Material::Material(const std::string& name, const std::shared_ptr<Shader>& shader)
    : m_Name(name), m_Shader(shader) {}

std::shared_ptr<Material> Material::Create(const std::string& name,
                                            const std::shared_ptr<Shader>& shader) {
    return std::make_shared<Material>(name, shader);
}

MaterialProperty* Material::FindProperty(const std::string& name) {
    auto it = m_PropertyIndex.find(name);
    if (it != m_PropertyIndex.end())
        return &m_Properties[it->second];
    return nullptr;
}

void Material::UpdatePropertyIndex(const std::string& name, size_t index) {
    m_PropertyIndex[name] = index;
}

void Material::UpdateHasTextures() {
    m_HasTextures = false;
    for (auto& prop : m_Properties) {
        if (prop.Type == MaterialPropertyType::Texture2D && prop.TextureRef) {
            m_HasTextures = true;
            return;
        }
    }
}

std::string Material::ComputeFlagName(const std::string& name) {
    if (name.size() > 2 && name[0] == 'u' && name[1] == '_')
        return "u_Has" + name.substr(2);
    return std::string();
}

void Material::SetFloat(const std::string& name, float value) {
    if (auto* p = FindProperty(name)) { p->FloatValue = value; return; }
    MaterialProperty prop;
    prop.Name = name; prop.Type = MaterialPropertyType::Float; prop.FloatValue = value;
    UpdatePropertyIndex(name, m_Properties.size());
    m_Properties.push_back(prop);
}

void Material::SetInt(const std::string& name, int value) {
    if (auto* p = FindProperty(name)) { p->IntValue = value; return; }
    MaterialProperty prop;
    prop.Name = name; prop.Type = MaterialPropertyType::Int; prop.IntValue = value;
    UpdatePropertyIndex(name, m_Properties.size());
    m_Properties.push_back(prop);
}

void Material::SetVec3(const std::string& name, const glm::vec3& value) {
    if (auto* p = FindProperty(name)) { p->Vec3Value = value; return; }
    MaterialProperty prop;
    prop.Name = name; prop.Type = MaterialPropertyType::Vec3; prop.Vec3Value = value;
    UpdatePropertyIndex(name, m_Properties.size());
    m_Properties.push_back(prop);
}

void Material::SetVec4(const std::string& name, const glm::vec4& value) {
    if (auto* p = FindProperty(name)) { p->Vec4Value = value; return; }
    MaterialProperty prop;
    prop.Name = name; prop.Type = MaterialPropertyType::Vec4; prop.Vec4Value = value;
    UpdatePropertyIndex(name, m_Properties.size());
    m_Properties.push_back(prop);
}

void Material::SetTexture(const std::string& name, const std::string& path) {
    if (auto* p = FindProperty(name)) {
        p->TexturePath = path;
        p->TextureRef = path.empty() ? nullptr : Texture2D::Create(path);
        UpdateHasTextures();
        return;
    }
    MaterialProperty prop;
    prop.Name = name; prop.Type = MaterialPropertyType::Texture2D;
    prop.TexturePath = path;
    prop.TextureRef = path.empty() ? nullptr : Texture2D::Create(path);
    prop.FlagName = ComputeFlagName(name);
    UpdatePropertyIndex(name, m_Properties.size());
    m_Properties.push_back(prop);
    UpdateHasTextures();
}

void Material::PopulateFromShader() {
    if (!m_Shader) return;
    for (const auto& info : m_Shader->GetPropertyInfos()) {
        if (FindProperty(info.Name)) continue; // don't overwrite existing

        MaterialProperty prop;
        prop.Name = info.Name;
        prop.DisplayName = info.DisplayName;

        switch (info.Type) {
            case ShaderPropertyType::Float:
                prop.Type = MaterialPropertyType::Float;
                prop.FloatValue = info.FloatDefault;
                break;
            case ShaderPropertyType::Range:
                prop.Type = MaterialPropertyType::Float;
                prop.FloatValue = info.FloatDefault;
                prop.IsRange = true;
                prop.RangeMin = info.RangeMin;
                prop.RangeMax = info.RangeMax;
                break;
            case ShaderPropertyType::Int:
                prop.Type = MaterialPropertyType::Int;
                prop.IntValue = info.IntDefault;
                break;
            case ShaderPropertyType::Color:
                prop.Type = MaterialPropertyType::Vec4;
                prop.Vec4Value = info.VectorDefault;
                prop.DisplayName = info.DisplayName;
                break;
            case ShaderPropertyType::Vector:
                prop.Type = MaterialPropertyType::Vec4;
                prop.Vec4Value = info.VectorDefault;
                break;
            case ShaderPropertyType::Texture2D:
                prop.Type = MaterialPropertyType::Texture2D;
                prop.TexturePath = info.TextureDefault;
                prop.FlagName = ComputeFlagName(info.Name);
                break;
        }
        UpdatePropertyIndex(prop.Name, m_Properties.size());
        m_Properties.push_back(std::move(prop));
    }
    UpdateHasTextures();
}

void Material::Bind() const {
    if (!m_Shader) return;
    m_Shader->Bind();
    m_Shader->ApplyRenderState();

    int texSlot = 0;
    bool hasMainTex = false;
    for (auto& prop : m_Properties) {
        switch (prop.Type) {
            case MaterialPropertyType::Float:
                m_Shader->SetFloat(prop.Name, prop.FloatValue);
                break;
            case MaterialPropertyType::Int:
                m_Shader->SetInt(prop.Name, prop.IntValue);
                break;
            case MaterialPropertyType::Vec3:
                m_Shader->SetVec3(prop.Name, prop.Vec3Value);
                break;
            case MaterialPropertyType::Vec4:
                m_Shader->SetVec4(prop.Name, prop.Vec4Value);
                break;
            case MaterialPropertyType::Texture2D: {
                bool bound = false;
                if (prop.TextureRef) {
                    prop.TextureRef->Bind(texSlot);
                    m_Shader->SetInt(prop.Name, texSlot);
                    texSlot++;
                    bound = true;
                    if (prop.Name == "u_MainTex" || prop.Name == "u_Texture")
                        hasMainTex = true;
                }
                // Set per-texture presence flag: u_MainTex -> u_HasMainTex
                if (!prop.FlagName.empty()) {
                    m_Shader->SetInt(prop.FlagName, bound ? 1 : 0);
                }
                break;
            }
        }
    }
    // Legacy backward compat for unlit shaders that use u_UseTexture
    m_Shader->SetInt("u_UseTexture", hasMainTex ? 1 : 0);
}

void Material::Save(const std::string& filePath) const {
    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "Material" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "Name" << YAML::Value << m_Name;
    out << YAML::Key << "IsLit" << YAML::Value << m_IsLit;
    // Save shader name for proper resolution on load (PBR, Lit, Default, etc.)
    if (m_Shader)
        out << YAML::Key << "ShaderName" << YAML::Value << m_Shader->GetName();
    out << YAML::Key << "Properties" << YAML::Value << YAML::BeginSeq;

    for (auto& prop : m_Properties) {
        out << YAML::BeginMap;
        out << YAML::Key << "Name" << YAML::Value << prop.Name;
        switch (prop.Type) {
            case MaterialPropertyType::Float:
                out << YAML::Key << "Type" << YAML::Value << "Float";
                out << YAML::Key << "Value" << YAML::Value << prop.FloatValue;
                break;
            case MaterialPropertyType::Int:
                out << YAML::Key << "Type" << YAML::Value << "Int";
                out << YAML::Key << "Value" << YAML::Value << prop.IntValue;
                break;
            case MaterialPropertyType::Vec3:
                out << YAML::Key << "Type" << YAML::Value << "Vec3";
                out << YAML::Key << "Value" << YAML::Value << YAML::Flow
                    << YAML::BeginSeq << prop.Vec3Value.x << prop.Vec3Value.y << prop.Vec3Value.z << YAML::EndSeq;
                break;
            case MaterialPropertyType::Vec4:
                out << YAML::Key << "Type" << YAML::Value << "Vec4";
                out << YAML::Key << "Value" << YAML::Value << YAML::Flow
                    << YAML::BeginSeq << prop.Vec4Value.x << prop.Vec4Value.y << prop.Vec4Value.z << prop.Vec4Value.w << YAML::EndSeq;
                break;
            case MaterialPropertyType::Texture2D:
                out << YAML::Key << "Type" << YAML::Value << "Texture2D";
                out << YAML::Key << "Value" << YAML::Value << prop.TexturePath;
                break;
        }
        out << YAML::EndMap;
    }

    out << YAML::EndSeq;
    out << YAML::EndMap;
    out << YAML::EndMap;

    std::ofstream fout(filePath);
    fout << out.c_str();
    VE_ENGINE_INFO("Material saved: {0}", filePath);
}

std::shared_ptr<Material> Material::Load(const std::string& filePath) {
    YAML::Node data;
    try {
        data = YAML::LoadFile(filePath);
    } catch (...) {
        VE_ENGINE_ERROR("Failed to load material: {0}", filePath);
        return nullptr;
    }

    auto matNode = data["Material"];
    if (!matNode) return nullptr;

    std::string name = matNode["Name"].as<std::string>("Unnamed");
    bool isLit = matNode["IsLit"].as<bool>(false);

    // Resolve shader: first try explicit ShaderName, then fall back to IsLit heuristic
    std::shared_ptr<Shader> shader;
    if (auto shaderNameNode = matNode["ShaderName"]) {
        std::string shaderName = shaderNameNode.as<std::string>();
        shader = ShaderLibrary::Get(shaderName);
    }
    if (!shader) {
        // Legacy fallback: resolve by IsLit flag
        auto litMat = MaterialLibrary::Get("Lit");
        auto defMat = MaterialLibrary::Get("Default");
        shader = (isLit && litMat) ? litMat->GetShader()
               : (defMat ? defMat->GetShader() : nullptr);
    }

    auto material = std::make_shared<Material>(name, shader);
    material->m_FilePath = filePath;
    material->m_IsLit = isLit;

    if (auto propsNode = matNode["Properties"]) {
        for (auto propNode : propsNode) {
            std::string propName = propNode["Name"].as<std::string>();
            std::string type = propNode["Type"].as<std::string>();

            if (type == "Float")
                material->SetFloat(propName, propNode["Value"].as<float>());
            else if (type == "Int")
                material->SetInt(propName, propNode["Value"].as<int>());
            else if (type == "Vec3") {
                auto v = propNode["Value"];
                material->SetVec3(propName, { v[0].as<float>(), v[1].as<float>(), v[2].as<float>() });
            } else if (type == "Vec4") {
                auto v = propNode["Value"];
                material->SetVec4(propName, { v[0].as<float>(), v[1].as<float>(), v[2].as<float>(), v[3].as<float>() });
            } else if (type == "Texture2D")
                material->SetTexture(propName, propNode["Value"].as<std::string>());
        }
    }

    VE_ENGINE_INFO("Material loaded: {0}", filePath);
    return material;
}

// ── MaterialLibrary ─────────────────────────────────────────────────

std::unordered_map<std::string, std::shared_ptr<Material>> MaterialLibrary::s_Materials;

void MaterialLibrary::Init() {
    // Built-in materials are created by MeshLibrary after shaders are compiled
}

void MaterialLibrary::Shutdown() {
    s_Materials.clear();
}

void MaterialLibrary::Register(const std::shared_ptr<Material>& material) {
    s_Materials[material->GetName()] = material;
}

std::shared_ptr<Material> MaterialLibrary::Get(const std::string& name) {
    auto it = s_Materials.find(name);
    return it != s_Materials.end() ? it->second : nullptr;
}

std::vector<std::string> MaterialLibrary::GetAllNames() {
    std::vector<std::string> names;
    for (auto& [name, _] : s_Materials)
        names.push_back(name);
    std::sort(names.begin(), names.end());
    return names;
}

} // namespace VE
