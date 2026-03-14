/*
 * Input — Polls GLFW for keyboard, mouse, and gamepad state each frame.
 *
 * Uses GLFW key/button callbacks for reliable edge detection instead of
 * polling all 349 key codes every frame.
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

// Scroll callback accumulator
static float s_ScrollAccum = 0.0f;

static void ScrollCallback(GLFWwindow*, double, double yOffset) {
    s_ScrollAccum += static_cast<float>(yOffset);
}

// Key callback — updates key state array
static void KeyCallback(GLFWwindow*, int key, int /*scancode*/, int action, int /*mods*/) {
    if (key < 0 || key >= static_cast<int>(KeyCode::MaxKey)) return;
    if (action == GLFW_PRESS)
        Input::s_KeysCurrent[key] = true;
    else if (action == GLFW_RELEASE)
        Input::s_KeysCurrent[key] = false;
    // GLFW_REPEAT: keep current state (true)
}

// Mouse button callback
static void MouseButtonCallback(GLFWwindow*, int button, int action, int /*mods*/) {
    if (button < 0 || button >= static_cast<int>(MouseButton::MaxButton)) return;
    Input::s_MouseCurrent[button] = (action == GLFW_PRESS);
}

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

    // Install callbacks — must be set BEFORE ImGui so ImGui chains to them
    glfwSetScrollCallback(window, ScrollCallback);
    glfwSetKeyCallback(window, KeyCallback);
    glfwSetMouseButtonCallback(window, MouseButtonCallback);
}

void Input::Update() {
    if (!s_Window) return;

    // ── Keyboard: copy current to previous for edge detection ────────
    // Key state is updated via KeyCallback, not by polling
    s_KeysPrevious = s_KeysCurrent;

    // ── Mouse buttons: copy current to previous ─────────────────────
    s_MousePrevious = s_MouseCurrent;

    // ── Mouse position ──────────────────────────────────────────────
    s_MousePosPrev = s_MousePos;
    double mx, my;
    glfwGetCursorPos(s_Window, &mx, &my);
    s_MousePos = { static_cast<float>(mx), static_cast<float>(my) };

    // ── Scroll ──────────────────────────────────────────────────────
    s_ScrollDelta = s_ScrollAccum;
    s_ScrollAccum = 0.0f;

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
                    // Apply deadzone
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
