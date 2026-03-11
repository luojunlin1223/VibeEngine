/*
 * Scene — Owns the ECS registry and manages entity lifecycle.
 *
 * The Scene is the top-level container for all game objects.
 * It provides methods to create/destroy entities and to render
 * all entities that have a MeshRendererComponent.
 */
#pragma once

#include "VibeEngine/Core/UUID.h"
#include "VibeEngine/Renderer/Texture.h"
#include "VibeEngine/Physics/PhysicsWorld.h"
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <string>
#include <array>
#include <memory>

namespace VE {

class Entity;

struct RenderPipelineSettings {
    // Sky
    bool SkyEnabled = true;
    std::array<float, 3> SkyTopColor    = { 0.4f, 0.7f, 1.0f };
    std::array<float, 3> SkyBottomColor = { 0.9f, 0.9f, 0.95f };
    std::shared_ptr<Texture2D> SkyTexture;
    std::string SkyTexturePath;
};

class Scene {
public:
    Scene() = default;
    ~Scene() = default;

    Entity CreateEntity(const std::string& name = "GameObject");
    Entity CreateEntityWithUUID(UUID uuid, const std::string& name = "GameObject");
    void DestroyEntity(Entity entity);
    void SetParent(entt::entity child, entt::entity parent);
    void RemoveParent(entt::entity child);

    // Compute world model matrix (walks parent chain)
    glm::mat4 GetWorldTransform(entt::entity entity) const;

    void OnUpdate(float deltaTime = 0.0f);
    void OnRenderSky(const glm::mat4& skyViewProjection);
    void OnRender(const glm::mat4& viewProjection,
                  const glm::vec3& cameraPos = glm::vec3(0.0f));

    void StartPhysics();
    void StopPhysics();
    bool IsPhysicsRunning() const { return m_PhysicsRunning; }

    template<typename... Components>
    auto GetAllEntitiesWith() { return m_Registry.view<Components...>(); }

    entt::registry& GetRegistry() { return m_Registry; }

    RenderPipelineSettings& GetPipelineSettings() { return m_PipelineSettings; }
    const RenderPipelineSettings& GetPipelineSettings() const { return m_PipelineSettings; }

private:
    entt::registry m_Registry;
    uint32_t m_EntityCounter = 0;
    RenderPipelineSettings m_PipelineSettings;

    std::unique_ptr<PhysicsWorld> m_PhysicsWorld;
    bool  m_PhysicsRunning = false;
    float m_PhysicsAccumulator = 0.0f;

    friend class Entity;
};

} // namespace VE
