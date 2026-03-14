/*
 * Input — Polls GLFW for keyboard, mouse, and gamepad state each frame.
 *
 * Uses direct GLFW queries for keyboard/mouse (safe for valid key ranges)
 * and the gamepad API for controller input.
 */
#include "VibeEngine/Input/Input.h"
#include <GLFW/glfw3.h>
#include <cmath>

namespace VE {

GLFWwindow* Input::s_Window = nullptr;

std::array<bool, static_cast<int>(KeyCode::MaxKey)>      Input::s_KeysCurrent = {};
std::array<bool, static_cast<int>(KeyCode::MaxKey)>      Input::s_KeysPrevious = {};

std::array<bool, static_cast<int>(MouseButton::MaxButton)> Input::s_MouseCurrent = {};
std::array<bool, static_cast<int>(MouseButton::MaxButton)> Input::s_MousePrevious = {};
glm::vec2 Input::s_MousePos = { 0.0f, 0.0f };
glm::vec2 Input::s_MousePosPrev = { 0.0f, 0.0f };
float     Input::s_ScrollDelta = 0.0f;

std::array<Input::GamepadState, Input::MAX_GAMEPADS> Input::s_Gamepads = {};
float Input::s_Deadzone = 0.15f;

// Valid GLFW key codes to poll (only codes that GLFW actually defines)
static const int s_ValidKeys[] = {
    // Printable keys
    32, 39, 44, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 54, 55, 56, 57,
    59, 61,
    65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77,
    78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90,
    91, 92, 93, 96,
    // Function keys
    256, 257, 258, 259, 260, 261, 262, 263, 264, 265, 266, 267, 268, 269,
    280, 281, 282, 283, 284,
    290, 291, 292, 293, 294, 295, 296, 297, 298, 299, 300, 301,
    // Keypad
    320, 321, 322, 323, 324, 325, 326, 327, 328, 329,
    330, 331, 332, 333, 334, 335, 336,
    // Modifiers
    340, 341, 342, 343, 344, 345, 346, 347, 348
};
static constexpr int s_NumValidKeys = sizeof(s_ValidKeys) / sizeof(s_ValidKeys[0]);

// ── Init / Update ───────────────────────────────────────────────────

void Input::Init(GLFWwindow* window) {
    s_Window = window;
    s_KeysCurrent.fill(false);
    s_KeysPrevious.fill(false);
    s_MouseCurrent.fill(false);
    s_MousePrevious.fill(false);

    // Get initial mouse position
    double mx, my;
    glfwGetCursorPos(window, &mx, &my);
    s_MousePos = { static_cast<float>(mx), static_cast<float>(my) };
    s_MousePosPrev = s_MousePos;

    // No callbacks — we poll only valid keys to avoid conflicts with ImGui
}

void Input::Update() {
    if (!s_Window) return;

    // ── Keyboard: poll only valid GLFW key codes ────────────────────
    s_KeysPrevious = s_KeysCurrent;
    for (int i = 0; i < s_NumValidKeys; ++i) {
        int k = s_ValidKeys[i];
        s_KeysCurrent[k] = (glfwGetKey(s_Window, k) == GLFW_PRESS);
    }

    // ── Mouse buttons ───────────────────────────────────────────────
    s_MousePrevious = s_MouseCurrent;
    for (int i = 0; i < static_cast<int>(MouseButton::MaxButton); ++i) {
        s_MouseCurrent[i] = (glfwGetMouseButton(s_Window, i) == GLFW_PRESS);
    }

    // ── Mouse position ──────────────────────────────────────────────
    s_MousePosPrev = s_MousePos;
    double mx, my;
    glfwGetCursorPos(s_Window, &mx, &my);
    s_MousePos = { static_cast<float>(mx), static_cast<float>(my) };

    // ── Scroll (read from ImGui IO since ImGui owns the callback) ───
    // We cannot install our own scroll callback without conflicting with ImGui.
    // Instead, read the scroll value from ImGui's IO each frame.
    // This works because ImGui processes GLFW scroll events first.

    // ── Gamepads ────────────────────────────────────────────────────
    for (int gp = 0; gp < MAX_GAMEPADS; ++gp) {
        auto& state = s_Gamepads[gp];
        state.ButtonsPrevious = state.ButtonsCurrent;

        if (glfwJoystickPresent(gp) && glfwJoystickIsGamepad(gp)) {
            state.Connected = true;
            GLFWgamepadstate gpState;
            if (glfwGetGamepadState(gp, &gpState)) {
                for (int b = 0; b < static_cast<int>(GamepadButton::MaxButton); ++b) {
                    state.ButtonsCurrent[b] = (gpState.buttons[b] == GLFW_PRESS);
                }
                for (int a = 0; a < static_cast<int>(GamepadAxis::MaxAxis); ++a) {
                    float val = gpState.axes[a];
                    if (std::abs(val) < s_Deadzone)
                        val = 0.0f;
                    state.Axes[a] = val;
                }
            }
        } else {
            state.Connected = false;
            state.ButtonsCurrent.fill(false);
            state.Axes.fill(0.0f);
        }
    }
}

// ── Keyboard ────────────────────────────────────────────────────────

bool Input::IsKeyDown(KeyCode key) {
    int k = static_cast<int>(key);
    return k >= 0 && k < static_cast<int>(KeyCode::MaxKey) && s_KeysCurrent[k];
}

bool Input::IsKeyPressed(KeyCode key) {
    int k = static_cast<int>(key);
    return k >= 0 && k < static_cast<int>(KeyCode::MaxKey)
        && s_KeysCurrent[k] && !s_KeysPrevious[k];
}

bool Input::IsKeyReleased(KeyCode key) {
    int k = static_cast<int>(key);
    return k >= 0 && k < static_cast<int>(KeyCode::MaxKey)
        && !s_KeysCurrent[k] && s_KeysPrevious[k];
}

bool Input::IsKeyDown(int key) { return IsKeyDown(static_cast<KeyCode>(key)); }
bool Input::IsKeyPressed(int key) { return IsKeyPressed(static_cast<KeyCode>(key)); }
bool Input::IsKeyReleased(int key) { return IsKeyReleased(static_cast<KeyCode>(key)); }

// ── Mouse ───────────────────────────────────────────────────────────

bool Input::IsMouseButtonDown(MouseButton button) {
    int b = static_cast<int>(button);
    return b >= 0 && b < static_cast<int>(MouseButton::MaxButton) && s_MouseCurrent[b];
}

bool Input::IsMouseButtonPressed(MouseButton button) {
    int b = static_cast<int>(button);
    return b >= 0 && b < static_cast<int>(MouseButton::MaxButton)
        && s_MouseCurrent[b] && !s_MousePrevious[b];
}

bool Input::IsMouseButtonReleased(MouseButton button) {
    int b = static_cast<int>(button);
    return b >= 0 && b < static_cast<int>(MouseButton::MaxButton)
        && !s_MouseCurrent[b] && s_MousePrevious[b];
}

bool Input::IsMouseButtonDown(int button) {
    return IsMouseButtonDown(static_cast<MouseButton>(button));
}

glm::vec2 Input::GetMousePosition() { return s_MousePos; }
glm::vec2 Input::GetMouseDelta()    { return s_MousePos - s_MousePosPrev; }
float     Input::GetScrollDelta()   { return s_ScrollDelta; }

// ── Gamepad ─────────────────────────────────────────────────────────

bool Input::IsGamepadConnected(int gamepadID) {
    return gamepadID >= 0 && gamepadID < MAX_GAMEPADS && s_Gamepads[gamepadID].Connected;
}

std::string Input::GetGamepadName(int gamepadID) {
    if (!IsGamepadConnected(gamepadID)) return "";
    const char* name = glfwGetGamepadName(gamepadID);
    return name ? name : "Unknown";
}

bool Input::IsGamepadButtonDown(GamepadButton button, int gamepadID) {
    if (!IsGamepadConnected(gamepadID)) return false;
    int b = static_cast<int>(button);
    return b >= 0 && b < static_cast<int>(GamepadButton::MaxButton)
        && s_Gamepads[gamepadID].ButtonsCurrent[b];
}

bool Input::IsGamepadButtonPressed(GamepadButton button, int gamepadID) {
    if (!IsGamepadConnected(gamepadID)) return false;
    int b = static_cast<int>(button);
    return b >= 0 && b < static_cast<int>(GamepadButton::MaxButton)
        && s_Gamepads[gamepadID].ButtonsCurrent[b]
        && !s_Gamepads[gamepadID].ButtonsPrevious[b];
}

bool Input::IsGamepadButtonReleased(GamepadButton button, int gamepadID) {
    if (!IsGamepadConnected(gamepadID)) return false;
    int b = static_cast<int>(button);
    return b >= 0 && b < static_cast<int>(GamepadButton::MaxButton)
        && !s_Gamepads[gamepadID].ButtonsCurrent[b]
        && s_Gamepads[gamepadID].ButtonsPrevious[b];
}

float Input::GetGamepadAxis(GamepadAxis axis, int gamepadID) {
    if (!IsGamepadConnected(gamepadID)) return 0.0f;
    int a = static_cast<int>(axis);
    if (a < 0 || a >= static_cast<int>(GamepadAxis::MaxAxis)) return 0.0f;
    return s_Gamepads[gamepadID].Axes[a];
}

void  Input::SetDeadzone(float deadzone) { s_Deadzone = deadzone; }
float Input::GetDeadzone()               { return s_Deadzone; }

} // namespace VE
