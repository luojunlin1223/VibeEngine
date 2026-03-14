/*
 * AnimStateMachine — State-based animation controller with transitions.
 *
 * Each state references a clip index. Transitions fire when parameter
 * conditions are met. Supports crossfade blending between states.
 * Parameters: Float, Int, Bool, Trigger (auto-resets after consumed).
 */
#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <variant>

namespace VE {

// ── Parameter ────────────────────────────────────────────────────────

enum class AnimParamType { Float, Int, Bool, Trigger };

struct AnimParameter {
    std::string Name;
    AnimParamType Type = AnimParamType::Bool;
    float  FloatValue = 0.0f;
    int    IntValue   = 0;
    bool   BoolValue  = false;
};

// ── Condition ────────────────────────────────────────────────────────

enum class AnimConditionOp { Greater, Less, Equals, NotEquals, IsTrue, IsFalse };

struct AnimCondition {
    std::string ParamName;
    AnimConditionOp Op = AnimConditionOp::IsTrue;
    float Threshold = 0.0f; // for Float/Int comparisons
};

// ── Transition ───────────────────────────────────────────────────────

struct AnimTransition {
    int FromState = -1;  // -1 = any state
    int ToState   = 0;
    float Duration = 0.2f;  // crossfade duration in seconds
    bool HasExitTime = false;
    float ExitTime   = 1.0f; // normalized (0-1) position in clip to auto-transition
    std::vector<AnimCondition> Conditions; // all must be true (AND)
};

// ── State ────────────────────────────────────────────────────────────

struct AnimState {
    std::string Name = "State";
    int ClipIndex = 0;
    float Speed   = 1.0f;
    bool  Loop    = true;
};

// ── State Machine ────────────────────────────────────────────────────

class AnimStateMachine {
public:
    AnimStateMachine() = default;

    // Build
    int  AddState(const AnimState& state);
    int  AddTransition(const AnimTransition& transition);
    void AddParameter(const AnimParameter& param);

    // Runtime
    void SetFloat(const std::string& name, float value);
    void SetInt(const std::string& name, int value);
    void SetBool(const std::string& name, bool value);
    void SetTrigger(const std::string& name);

    float GetFloat(const std::string& name) const;
    int   GetInt(const std::string& name) const;
    bool  GetBool(const std::string& name) const;

    // Called each frame by Animator — returns current clip index and blend info
    struct EvalResult {
        int   ClipA    = 0;     // primary clip
        int   ClipB    = -1;    // blend target (-1 = no blend)
        float BlendT   = 0.0f;  // 0 = fully A, 1 = fully B
        float SpeedA   = 1.0f;
        float SpeedB   = 1.0f;
        bool  LoopA    = true;
        bool  LoopB    = true;
        bool  StateChanged = false;
    };

    EvalResult Evaluate(float deltaTime, float currentClipTime, float currentClipDuration);

    // Accessors
    int GetCurrentState() const { return m_CurrentState; }
    const std::string& GetCurrentStateName() const;
    const std::vector<AnimState>& GetStates() const { return m_States; }
    const std::vector<AnimTransition>& GetTransitions() const { return m_Transitions; }
    const std::vector<AnimParameter>& GetParameters() const { return m_Parameters; }

    std::vector<AnimState>& GetStates() { return m_States; }
    std::vector<AnimTransition>& GetTransitions() { return m_Transitions; }
    std::vector<AnimParameter>& GetParameters() { return m_Parameters; }

    void SetDefaultState(int idx) { m_DefaultState = idx; }
    int  GetDefaultState() const { return m_DefaultState; }
    void Reset();

    bool IsTransitioning() const { return m_Transitioning; }

private:
    bool EvaluateCondition(const AnimCondition& cond) const;
    bool EvaluateTransition(const AnimTransition& trans, float normalizedTime) const;

    std::vector<AnimState>      m_States;
    std::vector<AnimTransition> m_Transitions;
    std::vector<AnimParameter>  m_Parameters;

    int   m_DefaultState  = 0;
    int   m_CurrentState  = 0;
    bool  m_Transitioning = false;
    int   m_TransitionTarget = -1;
    float m_TransitionTime   = 0.0f;
    float m_TransitionDuration = 0.2f;

    static const std::string s_EmptyName;
};

} // namespace VE
