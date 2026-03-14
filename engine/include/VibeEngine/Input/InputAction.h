/*
 * InputAction — Configurable input mapping system.
 *
 * Actions are named inputs (e.g., "Jump", "MoveHorizontal") that can be
 * bound to multiple physical inputs (keyboard, mouse, gamepad).
 * Action maps can be saved/loaded from YAML for user-configurable bindings.
 *
 * Design inspired by Unity's new Input System:
 *   - InputAction: a named input with a value
 *   - InputBinding: maps a physical input to an action
 *   - InputActionMap: a named group of actions (e.g., "Player", "UI")
 */
#pragma once

#include "KeyCodes.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

namespace VE {

// ── Binding source types ────────────────────────────────────────────

enum class BindingSource {
    Key,             // Keyboard key
    MouseButton,     // Mouse button
    GamepadButton,   // Gamepad button
    GamepadAxis,     // Gamepad analog axis
    MouseAxisX,      // Mouse X movement (delta)
    MouseAxisY,      // Mouse Y movement (delta)
    ScrollWheel      // Mouse scroll wheel
};

// ── A single binding from a physical input to an action ─────────────

struct InputBinding {
    BindingSource Source = BindingSource::Key;
    int           Code  = 0;      // KeyCode, MouseButton, GamepadButton, or GamepadAxis value
    int           GamepadID = 0;  // Which gamepad (0-3)
    float         Scale = 1.0f;   // Multiplier (use -1 for inverted axis or negative key)

    // For composite bindings (e.g., W/S for vertical axis)
    // Positive key adds +Scale, negative key adds -Scale
};

// ── Action type ─────────────────────────────────────────────────────

enum class ActionType {
    Button,  // Digital: 0 or 1 (pressed/not pressed)
    Axis     // Analog: -1 to 1 (or 0 to 1 for triggers)
};

// ── A named input action with one or more bindings ──────────────────

class InputAction {
public:
    InputAction() = default;
    InputAction(const std::string& name, ActionType type = ActionType::Button);

    const std::string& GetName() const { return m_Name; }
    ActionType GetType() const { return m_Type; }

    // Add bindings
    void AddBinding(const InputBinding& binding);
    void ClearBindings();
    const std::vector<InputBinding>& GetBindings() const { return m_Bindings; }

    // Query current state (call after Input::Update)
    float GetValue() const;             // Analog value (-1 to 1 for axes, 0/1 for buttons)
    bool  IsPressed() const;            // Just pressed this frame (edge)
    bool  IsReleased() const;           // Just released this frame (edge)
    bool  IsDown() const;               // Held down

    // Called by InputActionMap::Update()
    void Update();

private:
    std::string m_Name;
    ActionType  m_Type = ActionType::Button;
    std::vector<InputBinding> m_Bindings;

    float m_Value = 0.0f;
    float m_PrevValue = 0.0f;
};

// ── A named group of actions ────────────────────────────────────────

class InputActionMap {
public:
    InputActionMap() = default;
    InputActionMap(const std::string& name);

    const std::string& GetName() const { return m_Name; }

    // Add/get actions
    InputAction& AddAction(const std::string& name, ActionType type = ActionType::Button);
    InputAction* GetAction(const std::string& name);
    const std::vector<InputAction>& GetActions() const { return m_Actions; }
    std::vector<InputAction>& GetActions() { return m_Actions; }

    // Enable/disable the entire map
    void SetEnabled(bool enabled) { m_Enabled = enabled; }
    bool IsEnabled() const { return m_Enabled; }

    // Poll all actions (call once per frame)
    void Update();

    // Serialization
    bool SaveToFile(const std::string& path) const;
    bool LoadFromFile(const std::string& path);

private:
    std::string m_Name;
    std::vector<InputAction> m_Actions;
    bool m_Enabled = true;
};

// ── Global input action manager ─────────────────────────────────────

class InputActions {
public:
    static InputActionMap& CreateMap(const std::string& name);
    static InputActionMap* GetMap(const std::string& name);
    static void RemoveMap(const std::string& name);

    // Update all enabled maps
    static void UpdateAll();

    // Quick access: get action value by "MapName/ActionName"
    static float GetValue(const std::string& path);
    static bool  IsPressed(const std::string& path);
    static bool  IsDown(const std::string& path);

    static const std::vector<InputActionMap>& GetAllMaps() { return s_Maps; }
    static std::vector<InputActionMap>& GetAllMapsMutable() { return s_Maps; }

private:
    static std::vector<InputActionMap> s_Maps;
};

} // namespace VE
