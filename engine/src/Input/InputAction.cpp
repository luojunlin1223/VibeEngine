/*
 * InputAction — Evaluates bindings against the Input manager state,
 * and provides YAML save/load for configurable input maps.
 */
#include "VibeEngine/Input/InputAction.h"
#include "VibeEngine/Input/Input.h"
#include <yaml-cpp/yaml.h>
#include <fstream>
#include <cmath>

namespace VE {

// ── InputAction ─────────────────────────────────────────────────────

InputAction::InputAction(const std::string& name, ActionType type)
    : m_Name(name), m_Type(type) {}

void InputAction::AddBinding(const InputBinding& binding) {
    m_Bindings.push_back(binding);
}

void InputAction::ClearBindings() {
    m_Bindings.clear();
}

void InputAction::Update() {
    m_PrevValue = m_Value;
    float total = 0.0f;

    for (const auto& b : m_Bindings) {
        float contribution = 0.0f;

        switch (b.Source) {
            case BindingSource::Key:
                if (Input::IsKeyDown(b.Code))
                    contribution = 1.0f;
                break;

            case BindingSource::MouseButton:
                if (Input::IsMouseButtonDown(static_cast<MouseButton>(b.Code)))
                    contribution = 1.0f;
                break;

            case BindingSource::GamepadButton:
                if (Input::IsGamepadButtonDown(static_cast<GamepadButton>(b.Code), b.GamepadID))
                    contribution = 1.0f;
                break;

            case BindingSource::GamepadAxis:
                contribution = Input::GetGamepadAxis(static_cast<GamepadAxis>(b.Code), b.GamepadID);
                break;

            case BindingSource::MouseAxisX:
                contribution = Input::GetMouseDelta().x;
                break;

            case BindingSource::MouseAxisY:
                contribution = Input::GetMouseDelta().y;
                break;

            case BindingSource::ScrollWheel:
                contribution = Input::GetScrollDelta();
                break;
        }

        total += contribution * b.Scale;
    }

    // Clamp axis values
    if (m_Type == ActionType::Axis)
        total = std::max(-1.0f, std::min(1.0f, total));
    else
        total = (total > 0.5f) ? 1.0f : 0.0f;

    m_Value = total;
}

float InputAction::GetValue() const { return m_Value; }

bool InputAction::IsPressed() const {
    if (m_Type == ActionType::Button)
        return m_Value > 0.5f && m_PrevValue <= 0.5f;
    return std::abs(m_Value) > 0.5f && std::abs(m_PrevValue) <= 0.5f;
}

bool InputAction::IsReleased() const {
    if (m_Type == ActionType::Button)
        return m_Value <= 0.5f && m_PrevValue > 0.5f;
    return std::abs(m_Value) <= 0.5f && std::abs(m_PrevValue) > 0.5f;
}

bool InputAction::IsDown() const {
    return m_Type == ActionType::Button ? (m_Value > 0.5f) : (std::abs(m_Value) > 0.5f);
}

// ── InputActionMap ──────────────────────────────────────────────────

InputActionMap::InputActionMap(const std::string& name) : m_Name(name) {}

InputAction& InputActionMap::AddAction(const std::string& name, ActionType type) {
    m_Actions.emplace_back(name, type);
    return m_Actions.back();
}

InputAction* InputActionMap::GetAction(const std::string& name) {
    for (auto& a : m_Actions)
        if (a.GetName() == name) return &a;
    return nullptr;
}

void InputActionMap::Update() {
    if (!m_Enabled) return;
    for (auto& action : m_Actions)
        action.Update();
}

// ── Serialization helpers ───────────────────────────────────────────

static const char* SourceToString(BindingSource s) {
    switch (s) {
        case BindingSource::Key:           return "Key";
        case BindingSource::MouseButton:   return "MouseButton";
        case BindingSource::GamepadButton: return "GamepadButton";
        case BindingSource::GamepadAxis:   return "GamepadAxis";
        case BindingSource::MouseAxisX:    return "MouseAxisX";
        case BindingSource::MouseAxisY:    return "MouseAxisY";
        case BindingSource::ScrollWheel:   return "ScrollWheel";
    }
    return "Key";
}

static BindingSource StringToSource(const std::string& s) {
    if (s == "Key")           return BindingSource::Key;
    if (s == "MouseButton")   return BindingSource::MouseButton;
    if (s == "GamepadButton") return BindingSource::GamepadButton;
    if (s == "GamepadAxis")   return BindingSource::GamepadAxis;
    if (s == "MouseAxisX")    return BindingSource::MouseAxisX;
    if (s == "MouseAxisY")    return BindingSource::MouseAxisY;
    if (s == "ScrollWheel")   return BindingSource::ScrollWheel;
    return BindingSource::Key;
}

static const char* ActionTypeToString(ActionType t) {
    return t == ActionType::Button ? "Button" : "Axis";
}

static ActionType StringToActionType(const std::string& s) {
    return s == "Axis" ? ActionType::Axis : ActionType::Button;
}

bool InputActionMap::SaveToFile(const std::string& path) const {
    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "InputActionMap" << YAML::Value << m_Name;
    out << YAML::Key << "Actions" << YAML::Value << YAML::BeginSeq;

    for (const auto& action : m_Actions) {
        out << YAML::BeginMap;
        out << YAML::Key << "Name" << YAML::Value << action.GetName();
        out << YAML::Key << "Type" << YAML::Value << ActionTypeToString(action.GetType());
        out << YAML::Key << "Bindings" << YAML::Value << YAML::BeginSeq;

        for (const auto& binding : action.GetBindings()) {
            out << YAML::BeginMap;
            out << YAML::Key << "Source" << YAML::Value << SourceToString(binding.Source);
            out << YAML::Key << "Code"   << YAML::Value << binding.Code;
            if (binding.GamepadID != 0)
                out << YAML::Key << "GamepadID" << YAML::Value << binding.GamepadID;
            if (binding.Scale != 1.0f)
                out << YAML::Key << "Scale" << YAML::Value << binding.Scale;
            out << YAML::EndMap;
        }

        out << YAML::EndSeq;
        out << YAML::EndMap;
    }

    out << YAML::EndSeq;
    out << YAML::EndMap;

    std::ofstream fout(path);
    if (!fout.is_open()) return false;
    fout << out.c_str();
    return true;
}

bool InputActionMap::LoadFromFile(const std::string& path) {
    // Check file exists before attempting to parse
    {
        std::ifstream test(path);
        if (!test.is_open()) return false;
    }

    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch (...) {
        return false;
    }

    if (!root["InputActionMap"]) return false;
    m_Name = root["InputActionMap"].as<std::string>();
    m_Actions.clear();

    if (auto actionsNode = root["Actions"]) {
        for (auto actionNode : actionsNode) {
            std::string name = actionNode["Name"].as<std::string>();
            ActionType type = StringToActionType(actionNode["Type"].as<std::string>());
            auto& action = AddAction(name, type);

            if (auto bindingsNode = actionNode["Bindings"]) {
                for (auto bNode : bindingsNode) {
                    InputBinding b;
                    b.Source = StringToSource(bNode["Source"].as<std::string>());
                    b.Code   = bNode["Code"].as<int>();
                    if (bNode["GamepadID"]) b.GamepadID = bNode["GamepadID"].as<int>();
                    if (bNode["Scale"])     b.Scale = bNode["Scale"].as<float>();
                    action.AddBinding(b);
                }
            }
        }
    }
    return true;
}

// ── InputActions (global manager) ───────────────────────────────────

std::vector<InputActionMap> InputActions::s_Maps;

InputActionMap& InputActions::CreateMap(const std::string& name) {
    // Return existing if already created
    for (auto& m : s_Maps)
        if (m.GetName() == name) return m;
    s_Maps.emplace_back(name);
    return s_Maps.back();
}

InputActionMap* InputActions::GetMap(const std::string& name) {
    for (auto& m : s_Maps)
        if (m.GetName() == name) return &m;
    return nullptr;
}

void InputActions::RemoveMap(const std::string& name) {
    s_Maps.erase(
        std::remove_if(s_Maps.begin(), s_Maps.end(),
            [&](const InputActionMap& m) { return m.GetName() == name; }),
        s_Maps.end());
}

void InputActions::UpdateAll() {
    for (auto& map : s_Maps)
        map.Update();
}

// Parse "MapName/ActionName"
static bool ParsePath(const std::string& path, std::string& mapName, std::string& actionName) {
    auto slash = path.find('/');
    if (slash == std::string::npos) return false;
    mapName = path.substr(0, slash);
    actionName = path.substr(slash + 1);
    return true;
}

float InputActions::GetValue(const std::string& path) {
    std::string mapName, actionName;
    if (!ParsePath(path, mapName, actionName)) return 0.0f;
    auto* map = GetMap(mapName);
    if (!map || !map->IsEnabled()) return 0.0f;
    auto* action = map->GetAction(actionName);
    return action ? action->GetValue() : 0.0f;
}

bool InputActions::IsPressed(const std::string& path) {
    std::string mapName, actionName;
    if (!ParsePath(path, mapName, actionName)) return false;
    auto* map = GetMap(mapName);
    if (!map || !map->IsEnabled()) return false;
    auto* action = map->GetAction(actionName);
    return action ? action->IsPressed() : false;
}

bool InputActions::IsDown(const std::string& path) {
    std::string mapName, actionName;
    if (!ParsePath(path, mapName, actionName)) return false;
    auto* map = GetMap(mapName);
    if (!map || !map->IsEnabled()) return false;
    auto* action = map->GetAction(actionName);
    return action ? action->IsDown() : false;
}

} // namespace VE
