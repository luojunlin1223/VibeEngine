/*
 * Scene — Owns the ECS registry and manages entity lifecycle.
 *
 * The Scene is the top-level container for all game objects.
 * It provides methods to create/destroy entities and to render
 * all entities that have a MeshRendererComponent.
 */
#pragma once

#include <entt/entt.hpp>
#include <string>

namespace VE {

class Entity;

class Scene {
public:
    Scene() = default;
    ~Scene() = default;

    Entity CreateEntity(const std::string& name = "GameObject");
    void DestroyEntity(Entity entity);

    void OnUpdate();
    void OnRender();

    template<typename... Components>
    auto GetAllEntitiesWith() { return m_Registry.view<Components...>(); }

    entt::registry& GetRegistry() { return m_Registry; }

private:
    entt::registry m_Registry;
    uint32_t m_EntityCounter = 0;

    friend class Entity;
};

} // namespace VE
