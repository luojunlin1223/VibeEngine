/*
 * Input — Central input manager for keyboard, mouse, and gamepad.
 *
 * Polls GLFW each frame and tracks current/previous state for
 * press/release edge detection. Supports up to 4 gamepads.
 */
#pragma once

#include "KeyCodes.h"
#include <glm/glm.hpp>
#include <string>
#include <array>

struct GLFWwindow;

namespace VE {

class Input {
public:
    static constexpr int MAX_GAMEPADS = 4;

    // Call once at startup and once per frame
    static void Init(GLFWwindow* window);
    static void Update();

    // ── Keyboard ────────────────────────────────────────────────────
    static bool IsKeyDown(KeyCode key);          // held this frame
    static bool IsKeyPressed(KeyCode key);       // just pressed (edge)
    static bool IsKeyReleased(KeyCode key);      // just released (edge)

    // Raw int overloads (for backward compat with GLFW key codes)
    static bool IsKeyDown(int key);
    static bool IsKeyPressed(int key);
    static bool IsKeyReleased(int key);

    // ── Mouse ───────────────────────────────────────────────────────
    static bool IsMouseButtonDown(MouseButton button);
    static bool IsMouseButtonPressed(MouseButton button);
    static bool IsMouseButtonReleased(MouseButton button);

    static bool IsMouseButtonDown(int button);

    static glm::vec2 GetMousePosition();
    static glm::vec2 GetMouseDelta();
    static float     GetScrollDelta();

    // ── Gamepad ─────────────────────────────────────────────────────
    static bool        IsGamepadConnected(int gamepadID = 0);
    static std::string GetGamepadName(int gamepadID = 0);

    static bool  IsGamepadButtonDown(GamepadButton button, int gamepadID = 0);
    static bool  IsGamepadButtonPressed(GamepadButton button, int gamepadID = 0);
    static bool  IsGamepadButtonReleased(GamepadButton button, int gamepadID = 0);
    static float GetGamepadAxis(GamepadAxis axis, int gamepadID = 0);

    // Deadzone for analog sticks (default 0.15)
    static void  SetDeadzone(float deadzone);
    static float GetDeadzone();

    // Public for GLFW callback access (internal use only)
    static std::array<bool, static_cast<int>(KeyCode::MaxKey)> s_KeysCurrent;
    static std::array<bool, static_cast<int>(MouseButton::MaxButton)> s_MouseCurrent;

private:
    static GLFWwindow* s_Window;

    static std::array<bool, static_cast<int>(KeyCode::MaxKey)> s_KeysPrevious;

    static std::array<bool, static_cast<int>(MouseButton::MaxButton)> s_MousePrevious;
    static glm::vec2 s_MousePos;
    static glm::vec2 s_MousePosPrev;
    static float     s_ScrollDelta;

    // Gamepad state
    struct GamepadState {
        bool Connected = false;
        std::array<bool, static_cast<int>(GamepadButton::MaxButton)> ButtonsCurrent = {};
        std::array<bool, static_cast<int>(GamepadButton::MaxButton)> ButtonsPrevious = {};
        std::array<float, static_cast<int>(GamepadAxis::MaxAxis)>    Axes = {};
    };
    static std::array<GamepadState, MAX_GAMEPADS> s_Gamepads;
    static float s_Deadzone;
};

} // namespace VE
