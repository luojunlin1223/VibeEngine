/*
 * Entity — Lightweight wrapper around an entt entity handle.
 *
 * Provides a clean API for adding, getting, and checking components
 * without exposing the raw entt registry to user code.
 */
#pragma once

#include "Scene.h"
#include <entt/entt.hpp>
#include <cstdint>
#include <vector>

namespace VE {

class Entity {
public:
    Entity() = default;
    Entity(entt::entity handle, Scene* scene)
        : m_Handle(handle), m_Scene(scene) {}

    template<typename T, typename... Args>
    T& AddComponent(Args&&... args) {
        return m_Scene->GetRegistry().emplace<T>(m_Handle, std::forward<Args>(args)...);
    }

    template<typename T>
    T& GetComponent() {
        return m_Scene->GetRegistry().get<T>(m_Handle);
    }

    template<typename T>
    const T& GetComponent() const {
        return m_Scene->GetRegistry().get<T>(m_Handle);
    }

    template<typename T>
    bool HasComponent() const {
        return m_Scene->GetRegistry().all_of<T>(m_Handle);
    }

    template<typename T>
    void RemoveComponent() {
        m_Scene->GetRegistry().remove<T>(m_Handle);
    }

    bool IsValid() const {
        return m_Handle != entt::null && m_Scene != nullptr
            && m_Scene->GetRegistry().valid(m_Handle);
    }

    operator bool() const { return IsValid(); }
    operator entt::entity() const { return m_Handle; }
    operator uint32_t() const { return static_cast<uint32_t>(m_Handle); }

    bool operator==(const Entity& other) const {
        return m_Handle == other.m_Handle && m_Scene == other.m_Scene;
    }
    bool operator!=(const Entity& other) const { return !(*this == other); }

    entt::entity GetHandle() const { return m_Handle; }
    Scene* GetScene() const { return m_Scene; }

    // ── Hierarchy helpers (require Components.h to be included at call site) ──

    void SetParent(Entity parent) {
        if (m_Scene) m_Scene->SetParent(m_Handle, parent.m_Handle);
    }

    void RemoveParent() {
        if (m_Scene) m_Scene->RemoveParent(m_Handle);
    }

    Entity GetParent() const;

    std::vector<Entity> GetChildren() const;

    glm::mat4 GetWorldTransform() const {
        if (m_Scene) return m_Scene->GetWorldTransform(m_Handle);
        return glm::mat4(1.0f);
    }

private:
    entt::entity m_Handle = entt::null;
    Scene* m_Scene = nullptr;
};

} // namespace VE
