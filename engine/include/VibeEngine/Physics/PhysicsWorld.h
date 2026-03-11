/*
 * PhysicsWorld — Wraps Jolt Physics for 3D rigid body simulation.
 *
 * Uses PIMPL to keep Jolt headers out of the public API.
 * The Scene owns a PhysicsWorld and drives it from OnUpdate().
 */
#pragma once

#include <entt/entt.hpp>
#include <memory>

namespace VE {

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

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace VE
