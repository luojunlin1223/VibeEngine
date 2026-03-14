/*
 * PhysicsWorld — Wraps Jolt Physics for 3D rigid body simulation.
 *
 * Uses PIMPL to keep Jolt headers out of the public API.
 * The Scene owns a PhysicsWorld and drives it from OnUpdate().
 */
#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <memory>
#include <vector>
#include <cstdint>

namespace VE {

struct RaycastHit {
    glm::vec3 Point  = glm::vec3(0.0f);
    glm::vec3 Normal = glm::vec3(0.0f, 1.0f, 0.0f);
    float     Distance = 0.0f;
    uint32_t  BodyID = 0xFFFFFFFF;
};

struct CollisionEvent {
    uint32_t BodyA = 0xFFFFFFFF;
    uint32_t BodyB = 0xFFFFFFFF;
    glm::vec3 ContactPoint  = glm::vec3(0.0f);
    glm::vec3 ContactNormal = glm::vec3(0.0f, 1.0f, 0.0f);
    bool IsEnter = true; // true = enter, false = exit
};

class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();

    // Create/update Jolt bodies from entities that have Rigidbody + Collider + Transform
    void SyncBodiesFromScene(entt::registry& registry);

    // Step the physics simulation by deltaTime seconds
    void Step(float deltaTime);

    // Write Jolt body transforms back to TransformComponents (dynamic bodies only)
    void SyncTransformsToScene(entt::registry& registry);

    // Remove a Jolt body by its ID (called when entity is destroyed)
    void RemoveBody(uint32_t joltBodyID);

    void SetGravity(float x, float y, float z);

    // ── Physics queries ─────────────────────────────────────────────
    bool Raycast(const glm::vec3& origin, const glm::vec3& direction,
                 float maxDistance, RaycastHit& outHit) const;

    // ── Body manipulation ───────────────────────────────────────────
    void AddForce(uint32_t bodyID, const glm::vec3& force);
    void AddImpulse(uint32_t bodyID, const glm::vec3& impulse);
    void SetLinearVelocity(uint32_t bodyID, const glm::vec3& velocity);
    glm::vec3 GetLinearVelocity(uint32_t bodyID) const;
    void SetAngularVelocity(uint32_t bodyID, const glm::vec3& velocity);

    // ── Collision events ────────────────────────────────────────────
    const std::vector<CollisionEvent>& GetCollisionEvents() const;
    void ClearCollisionEvents();

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace VE
