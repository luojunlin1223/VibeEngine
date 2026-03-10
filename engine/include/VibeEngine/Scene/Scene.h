/*
 * Scene — Owns the ECS registry and manages entity lifecycle.
 *
 * The Scene is the top-level container for all game objects.
 * It provides methods to create/destroy entities and to render
 * all entities that have a MeshRendererComponent.
 */
#pragma once

#include "VibeEngine/Core/UUID.h"
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <string>

namespace VE {

class Entity;

class Scene {
public:
    Scene() = default;
    ~Scene() = default;

    Entity CreateEntity(const std::string& name = "GameObject");
    Entity CreateEntityWithUUID(UUID uuid, const std::string& name = "GameObject");
    void DestroyEntity(Entity entity);

    void OnUpdate();
    void OnRender(const glm::mat4& viewProjection,
                  const glm::vec3& cameraPos = glm::vec3(0.0f));

    template<typename... Components>
    auto GetAllEntitiesWith() { return m_Registry.view<Components...>(); }

    entt::registry& GetRegistry() { return m_Registry; }

private:
    entt::registry m_Registry;
    uint32_t m_EntityCounter = 0;

    friend class Entity;
};

} // namespace VE
