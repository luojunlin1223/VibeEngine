/*
 * AnimStateMachine — evaluates transitions and manages state changes.
 *
 * Each frame, Evaluate() checks all transitions from the current state
 * (and "any state" transitions). If conditions are met, starts a crossfade.
 * Returns blend info so the Animator can interpolate between two poses.
 */
#include "VibeEngine/Animation/AnimStateMachine.h"
#include <algorithm>

namespace VE {

const std::string AnimStateMachine::s_EmptyName = "";

int AnimStateMachine::AddState(const AnimState& state) {
    m_States.push_back(state);
    return static_cast<int>(m_States.size()) - 1;
}

int AnimStateMachine::AddTransition(const AnimTransition& transition) {
    m_Transitions.push_back(transition);
    return static_cast<int>(m_Transitions.size()) - 1;
}

void AnimStateMachine::AddParameter(const AnimParameter& param) {
    m_Parameters.push_back(param);
}

// ── Parameter setters ────────────────────────────────────────────────

void AnimStateMachine::SetFloat(const std::string& name, float value) {
    for (auto& p : m_Parameters)
        if (p.Name == name && p.Type == AnimParamType::Float) { p.FloatValue = value; return; }
}

void AnimStateMachine::SetInt(const std::string& name, int value) {
    for (auto& p : m_Parameters)
        if (p.Name == name && p.Type == AnimParamType::Int) { p.IntValue = value; return; }
}

void AnimStateMachine::SetBool(const std::string& name, bool value) {
    for (auto& p : m_Parameters)
        if (p.Name == name && p.Type == AnimParamType::Bool) { p.BoolValue = value; return; }
}

void AnimStateMachine::SetTrigger(const std::string& name) {
    for (auto& p : m_Parameters)
        if (p.Name == name && p.Type == AnimParamType::Trigger) { p.BoolValue = true; return; }
}

// ── Parameter getters ────────────────────────────────────────────────

float AnimStateMachine::GetFloat(const std::string& name) const {
    for (auto& p : m_Parameters)
        if (p.Name == name) return p.FloatValue;
    return 0.0f;
}

int AnimStateMachine::GetInt(const std::string& name) const {
    for (auto& p : m_Parameters)
        if (p.Name == name) return p.IntValue;
    return 0;
}

bool AnimStateMachine::GetBool(const std::string& name) const {
    for (auto& p : m_Parameters)
        if (p.Name == name) return p.BoolValue;
    return false;
}

// ── Condition evaluation ─────────────────────────────────────────────

bool AnimStateMachine::EvaluateCondition(const AnimCondition& cond) const {
    const AnimParameter* param = nullptr;
    for (auto& p : m_Parameters)
        if (p.Name == cond.ParamName) { param = &p; break; }
    if (!param) return false;

    switch (param->Type) {
        case AnimParamType::Float:
            switch (cond.Op) {
                case AnimConditionOp::Greater:   return param->FloatValue > cond.Threshold;
                case AnimConditionOp::Less:      return param->FloatValue < cond.Threshold;
                case AnimConditionOp::Equals:    return std::abs(param->FloatValue - cond.Threshold) < 0.001f;
                case AnimConditionOp::NotEquals:  return std::abs(param->FloatValue - cond.Threshold) >= 0.001f;
                default: return false;
            }
        case AnimParamType::Int:
            switch (cond.Op) {
                case AnimConditionOp::Greater:   return param->IntValue > static_cast<int>(cond.Threshold);
                case AnimConditionOp::Less:      return param->IntValue < static_cast<int>(cond.Threshold);
                case AnimConditionOp::Equals:    return param->IntValue == static_cast<int>(cond.Threshold);
                case AnimConditionOp::NotEquals:  return param->IntValue != static_cast<int>(cond.Threshold);
                default: return false;
            }
        case AnimParamType::Bool:
            if (cond.Op == AnimConditionOp::IsTrue)  return param->BoolValue;
            if (cond.Op == AnimConditionOp::IsFalse) return !param->BoolValue;
            return false;
        case AnimParamType::Trigger:
            return param->BoolValue; // triggers are consumed after transition fires
    }
    return false;
}

bool AnimStateMachine::EvaluateTransition(const AnimTransition& trans, float normalizedTime) const {
    // Check exit time
    if (trans.HasExitTime && normalizedTime < trans.ExitTime)
        return false;

    // All conditions must pass (AND)
    for (auto& cond : trans.Conditions)
        if (!EvaluateCondition(cond))
            return false;

    // If no conditions and no exit time, don't auto-fire
    if (trans.Conditions.empty() && !trans.HasExitTime)
        return false;

    return true;
}

// ── Core evaluation ──────────────────────────────────────────────────

AnimStateMachine::EvalResult AnimStateMachine::Evaluate(float deltaTime,
                                                         float currentClipTime,
                                                         float currentClipDuration) {
    EvalResult result;

    if (m_States.empty()) {
        result.ClipA = 0;
        return result;
    }

    int cur = std::clamp(m_CurrentState, 0, static_cast<int>(m_States.size()) - 1);
    auto& curState = m_States[cur];
    result.ClipA  = curState.ClipIndex;
    result.SpeedA = curState.Speed;
    result.LoopA  = curState.Loop;

    float normalizedTime = (currentClipDuration > 0.0f)
        ? currentClipTime / currentClipDuration : 0.0f;

    // ── If currently transitioning, advance blend ────────────────────
    if (m_Transitioning) {
        m_TransitionTime += deltaTime;
        float t = (m_TransitionDuration > 0.0f)
            ? std::clamp(m_TransitionTime / m_TransitionDuration, 0.0f, 1.0f) : 1.0f;

        if (t >= 1.0f) {
            // Transition complete
            m_CurrentState = m_TransitionTarget;
            m_Transitioning = false;
            cur = m_CurrentState;
            auto& newState = m_States[cur];
            result.ClipA  = newState.ClipIndex;
            result.SpeedA = newState.Speed;
            result.LoopA  = newState.Loop;
            result.ClipB  = -1;
            result.BlendT = 0.0f;
            result.StateChanged = true;
        } else {
            // Mid-transition: output blend
            auto& targetState = m_States[m_TransitionTarget];
            result.ClipA  = curState.ClipIndex;
            result.SpeedA = curState.Speed;
            result.LoopA  = curState.Loop;
            result.ClipB  = targetState.ClipIndex;
            result.SpeedB = targetState.Speed;
            result.LoopB  = targetState.Loop;
            result.BlendT = t;
        }
        return result;
    }

    // ── Check transitions ────────────────────────────────────────────
    for (auto& trans : m_Transitions) {
        // Must match current state or be "any state" (-1)
        if (trans.FromState != -1 && trans.FromState != cur)
            continue;
        // Don't transition to self (unless it's a different state index)
        if (trans.ToState == cur)
            continue;
        if (trans.ToState < 0 || trans.ToState >= static_cast<int>(m_States.size()))
            continue;

        if (EvaluateTransition(trans, normalizedTime)) {
            // Fire transition
            m_Transitioning = true;
            m_TransitionTarget = trans.ToState;
            m_TransitionTime = 0.0f;
            m_TransitionDuration = trans.Duration;

            // Consume triggers
            for (auto& cond : trans.Conditions) {
                for (auto& p : m_Parameters) {
                    if (p.Name == cond.ParamName && p.Type == AnimParamType::Trigger)
                        p.BoolValue = false;
                }
            }

            auto& targetState = m_States[trans.ToState];
            result.ClipB  = targetState.ClipIndex;
            result.SpeedB = targetState.Speed;
            result.LoopB  = targetState.Loop;
            result.BlendT = 0.0f;
            result.StateChanged = true;
            break;
        }
    }

    return result;
}

const std::string& AnimStateMachine::GetCurrentStateName() const {
    if (m_CurrentState >= 0 && m_CurrentState < static_cast<int>(m_States.size()))
        return m_States[m_CurrentState].Name;
    return s_EmptyName;
}

void AnimStateMachine::Reset() {
    m_CurrentState = m_DefaultState;
    m_Transitioning = false;
    m_TransitionTarget = -1;
    m_TransitionTime = 0.0f;
    for (auto& p : m_Parameters) {
        if (p.Type == AnimParamType::Trigger) p.BoolValue = false;
    }
}

} // namespace VE
