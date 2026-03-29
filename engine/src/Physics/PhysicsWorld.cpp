/*
 * PhysicsWorld — Jolt Physics integration.
 *
 * Manages the Jolt PhysicsSystem, body creation, stepping, and transform sync.
 * All Jolt headers are confined to this translation unit via PIMPL.
 */

#include "VibeEngine/Physics/PhysicsWorld.h"
#include "VibeEngine/Scene/Components.h"
#include "VibeEngine/Core/Log.h"

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/ContactListener.h>

#include "VibeEngine/Renderer/VertexArray.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/quaternion.hpp>

#include <cmath>
#include <cstdarg>

// Jolt uses its own trace/assert hooks
JPH_SUPPRESS_WARNINGS

// ── Object layers ──────────────────────────────────────────────────
namespace Layers {
    static constexpr JPH::ObjectLayer NON_MOVING = 0;
    static constexpr JPH::ObjectLayer MOVING     = 1;
    static constexpr JPH::ObjectLayer NUM_LAYERS = 2;
}

namespace BroadPhaseLayers {
    static constexpr JPH::BroadPhaseLayer NON_MOVING(0);
    static constexpr JPH::BroadPhaseLayer MOVING(1);
    static constexpr uint32_t NUM_LAYERS = 2;
}

// ── Jolt callbacks ──────────────────────────────────────────────────

class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
public:
    uint32_t GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::NUM_LAYERS; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override {
        return inLayer == Layers::NON_MOVING ? BroadPhaseLayers::NON_MOVING : BroadPhaseLayers::MOVING;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override {
        switch ((JPH::BroadPhaseLayer::Type)inLayer) {
            case 0: return "NON_MOVING";
            case 1: return "MOVING";
            default: return "UNKNOWN";
        }
    }
#endif
};

class ObjectVsBroadPhaseLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const override {
        if (inLayer1 == Layers::NON_MOVING)
            return inLayer2 == BroadPhaseLayers::MOVING;
        return true;
    }
};

class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer inObject1, JPH::ObjectLayer inObject2) const override {
        if (inObject1 == Layers::NON_MOVING && inObject2 == Layers::NON_MOVING)
            return false;
        return true;
    }
};

// ── GLM <-> Jolt conversions ────────────────────────────────────────

static JPH::Vec3 ToJolt(const glm::vec3& v) { return JPH::Vec3(v.x, v.y, v.z); }
static glm::vec3 FromJolt(JPH::Vec3 v) { return glm::vec3(v.GetX(), v.GetY(), v.GetZ()); }

static JPH::Quat EulerDegreesToJoltQuat(const std::array<float, 3>& euler) {
    glm::quat q = glm::quat(glm::vec3(
        glm::radians(euler[0]),
        glm::radians(euler[1]),
        glm::radians(euler[2])
    ));
    return JPH::Quat(q.x, q.y, q.z, q.w);
}

static std::array<float, 3> JoltQuatToEulerDegrees(JPH::Quat q) {
    glm::quat gq(q.GetW(), q.GetX(), q.GetY(), q.GetZ());
    glm::vec3 euler = glm::degrees(glm::eulerAngles(gq));
    return { euler.x, euler.y, euler.z };
}

// ── PhysicsWorld::Impl ──────────────────────────────────────────────

namespace VE {

// ── Contact listener for collision events ───────────────────────────

class VEContactListener : public JPH::ContactListener {
public:
    std::vector<CollisionEvent>* Events = nullptr;

    JPH::ValidateResult OnContactValidate(const JPH::Body&, const JPH::Body&,
        JPH::RVec3Arg, const JPH::CollideShapeResult&) override {
        return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
    }

    void OnContactAdded(const JPH::Body& b1, const JPH::Body& b2,
        const JPH::ContactManifold& manifold, JPH::ContactSettings&) override {
        if (!Events) return;
        CollisionEvent ev;
        ev.BodyA = b1.GetID().GetIndexAndSequenceNumber();
        ev.BodyB = b2.GetID().GetIndexAndSequenceNumber();
        auto cp = manifold.GetWorldSpaceContactPointOn1(0);
        ev.ContactPoint = glm::vec3(cp.GetX(), cp.GetY(), cp.GetZ());
        auto cn = manifold.mWorldSpaceNormal;
        ev.ContactNormal = glm::vec3(cn.GetX(), cn.GetY(), cn.GetZ());
        ev.IsEnter = true;
        Events->push_back(ev);
    }

    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
        if (!Events) return;
        CollisionEvent ev;
        ev.BodyA = pair.GetBody1ID().GetIndexAndSequenceNumber();
        ev.BodyB = pair.GetBody2ID().GetIndexAndSequenceNumber();
        ev.IsEnter = false;
        Events->push_back(ev);
    }
};

struct PhysicsWorld::Impl {
    std::unique_ptr<JPH::TempAllocatorImpl>     TempAllocator;
    std::unique_ptr<JPH::JobSystemThreadPool>    JobSystem;
    std::unique_ptr<JPH::PhysicsSystem>          PhysicsSystem;

    BPLayerInterfaceImpl                  BPLayerInterface;
    ObjectVsBroadPhaseLayerFilterImpl     ObjVsBPFilter;
    ObjectLayerPairFilterImpl             ObjPairFilter;

    VEContactListener ContactListener;
    std::vector<CollisionEvent> CollisionEvents;
};

// ── Static Jolt init (called once) ──────────────────────────────────

static bool s_JoltInitialized = false;

static void InitJoltOnce() {
    if (s_JoltInitialized) return;
    s_JoltInitialized = true;

    JPH::RegisterDefaultAllocator();

    // Jolt trace callback
    JPH::Trace = [](const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        char buf[1024];
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        VE_ENGINE_INFO("[Jolt] {0}", buf);
    };

    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
}

// ── PhysicsWorld implementation ─────────────────────────────────────

PhysicsWorld::PhysicsWorld() : m_Impl(std::make_unique<Impl>()) {
    InitJoltOnce();

    m_Impl->TempAllocator = std::make_unique<JPH::TempAllocatorImpl>(10 * 1024 * 1024);
    m_Impl->JobSystem     = std::make_unique<JPH::JobSystemThreadPool>(
        JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
        static_cast<int>(std::max(1u, std::thread::hardware_concurrency() - 1)));

    const uint32_t maxBodies     = 4096;
    const uint32_t numBodyMutexes = 0; // auto
    const uint32_t maxBodyPairs  = 4096;
    const uint32_t maxContactConstraints = 2048;

    m_Impl->PhysicsSystem = std::make_unique<JPH::PhysicsSystem>();
    m_Impl->PhysicsSystem->Init(
        maxBodies, numBodyMutexes, maxBodyPairs, maxContactConstraints,
        m_Impl->BPLayerInterface, m_Impl->ObjVsBPFilter, m_Impl->ObjPairFilter);

    m_Impl->PhysicsSystem->SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));

    // Register contact listener for collision events
    m_Impl->ContactListener.Events = &m_Impl->CollisionEvents;
    m_Impl->PhysicsSystem->SetContactListener(&m_Impl->ContactListener);

    VE_ENGINE_INFO("PhysicsWorld initialized (Jolt Physics)");
}

PhysicsWorld::~PhysicsWorld() {
    VE_ENGINE_INFO("PhysicsWorld destroyed");
}

void PhysicsWorld::SetGravity(float x, float y, float z) {
    m_Impl->PhysicsSystem->SetGravity(JPH::Vec3(x, y, z));
}

// Helper: resolve collider shape and offset from whichever collider component is present
static bool ResolveCollider(entt::registry& registry, entt::entity entity,
                             JPH::ShapeRefC& outShape, std::array<float, 3>& outOffset) {
    if (auto* box = registry.try_get<BoxColliderComponent>(entity)) {
        outShape = new JPH::BoxShape(JPH::Vec3(
            box->Size[0] * 0.5f, box->Size[1] * 0.5f, box->Size[2] * 0.5f));
        outOffset = box->Offset;
        return true;
    }
    if (auto* sphere = registry.try_get<SphereColliderComponent>(entity)) {
        outShape = new JPH::SphereShape(sphere->Radius);
        outOffset = sphere->Offset;
        return true;
    }
    if (auto* capsule = registry.try_get<CapsuleColliderComponent>(entity)) {
        // Jolt CapsuleShape: half-height of cylinder part + radius
        float halfCylinder = std::max(0.0f, capsule->Height * 0.5f - capsule->Radius);
        outShape = new JPH::CapsuleShape(halfCylinder, capsule->Radius);
        outOffset = capsule->Offset;
        return true;
    }
    if (auto* mesh = registry.try_get<MeshColliderComponent>(entity)) {
        // Get mesh vertices from MeshRendererComponent
        auto* mr = registry.try_get<MeshRendererComponent>(entity);
        if (!mr || !mr->Mesh) {
            VE_ENGINE_WARN("MeshCollider: entity has no mesh — falling back to unit box");
            outShape = new JPH::BoxShape(JPH::Vec3(0.5f, 0.5f, 0.5f));
            outOffset = mesh->Offset;
            return true;
        }

        // For now, use a box approximation based on mesh bounds
        // TODO: extract actual vertex data for ConvexHull / TriangleMesh
        outShape = new JPH::BoxShape(JPH::Vec3(0.5f, 0.5f, 0.5f));
        outOffset = mesh->Offset;
        VE_ENGINE_WARN("MeshCollider: using box approximation (full mesh collision WIP)");
        return true;
    }
    return false;
}

void PhysicsWorld::SyncBodiesFromScene(entt::registry& registry) {
    auto& bodyInterface = m_Impl->PhysicsSystem->GetBodyInterface();

    auto view = registry.view<TransformComponent, RigidbodyComponent>();
    for (auto entity : view) {
        auto& tc = view.get<TransformComponent>(entity);
        auto& rb = view.get<RigidbodyComponent>(entity);

        // Resolve collider
        JPH::ShapeRefC shape;
        std::array<float, 3> offset = { 0.0f, 0.0f, 0.0f };
        if (!ResolveCollider(registry, entity, shape, offset))
            continue; // no collider component

        // Already has a Jolt body
        if (rb._JoltBodyID != 0xFFFFFFFF) {
            if (rb.Type == BodyType::Kinematic) {
                JPH::BodyID id(rb._JoltBodyID);
                bodyInterface.SetPosition(id,
                    JPH::Vec3(tc.Position[0] + offset[0],
                              tc.Position[1] + offset[1],
                              tc.Position[2] + offset[2]),
                    JPH::EActivation::DontActivate);
                bodyInterface.SetRotation(id,
                    EulerDegreesToJoltQuat(tc.Rotation),
                    JPH::EActivation::DontActivate);
            }
            continue;
        }

        // Map body type
        JPH::EMotionType motionType;
        JPH::ObjectLayer layer;
        switch (rb.Type) {
            case BodyType::Static:
                motionType = JPH::EMotionType::Static;
                layer = Layers::NON_MOVING;
                break;
            case BodyType::Kinematic:
                motionType = JPH::EMotionType::Kinematic;
                layer = Layers::MOVING;
                break;
            default:
                motionType = JPH::EMotionType::Dynamic;
                layer = Layers::MOVING;
                break;
        }

        JPH::BodyCreationSettings bodySettings(
            shape,
            JPH::Vec3(tc.Position[0] + offset[0],
                       tc.Position[1] + offset[1],
                       tc.Position[2] + offset[2]),
            EulerDegreesToJoltQuat(tc.Rotation),
            motionType,
            layer);

        if (rb.Type == BodyType::Dynamic) {
            bodySettings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            bodySettings.mMassPropertiesOverride.mMass = rb.Mass;
        }
        bodySettings.mRestitution   = rb.Restitution;
        bodySettings.mFriction      = rb.Friction;
        bodySettings.mLinearDamping = rb.LinearDamping;
        bodySettings.mAngularDamping = rb.AngularDamping;
        bodySettings.mGravityFactor = rb.UseGravity ? 1.0f : 0.0f;

        JPH::Body* body = bodyInterface.CreateBody(bodySettings);
        if (!body) {
            VE_ENGINE_ERROR("Failed to create Jolt body!");
            continue;
        }

        rb._JoltBodyID = body->GetID().GetIndexAndSequenceNumber();
        bodyInterface.AddBody(body->GetID(), JPH::EActivation::Activate);
    }
}

void PhysicsWorld::Step(float deltaTime) {
    // Jolt recommends collision steps = 1 for simple simulations
    m_Impl->PhysicsSystem->Update(deltaTime, 1,
        m_Impl->TempAllocator.get(), m_Impl->JobSystem.get());
}

void PhysicsWorld::SyncTransformsToScene(entt::registry& registry) {
    auto& bodyInterface = m_Impl->PhysicsSystem->GetBodyInterface();

    auto view = registry.view<TransformComponent, RigidbodyComponent>();
    for (auto entity : view) {
        auto& tc = view.get<TransformComponent>(entity);
        auto& rb = view.get<RigidbodyComponent>(entity);

        if (rb._JoltBodyID == 0xFFFFFFFF) continue;
        if (rb.Type != BodyType::Dynamic) continue;

        JPH::BodyID id(rb._JoltBodyID);
        JPH::Vec3 pos = bodyInterface.GetPosition(id);
        JPH::Quat rot = bodyInterface.GetRotation(id);

        // Subtract collider offset if entity has one
        std::array<float, 3> offset = { 0.0f, 0.0f, 0.0f };
        if (auto* box = registry.try_get<BoxColliderComponent>(entity))
            offset = box->Offset;
        else if (auto* sphere = registry.try_get<SphereColliderComponent>(entity))
            offset = sphere->Offset;
        else if (auto* capsule = registry.try_get<CapsuleColliderComponent>(entity))
            offset = capsule->Offset;
        else if (auto* mesh = registry.try_get<MeshColliderComponent>(entity))
            offset = mesh->Offset;

        tc.Position = { pos.GetX() - offset[0],
                        pos.GetY() - offset[1],
                        pos.GetZ() - offset[2] };

        tc.Rotation = JoltQuatToEulerDegrees(rot);
    }
}

void PhysicsWorld::RemoveBody(uint32_t joltBodyID) {
    if (joltBodyID == 0xFFFFFFFF) {
        VE_ENGINE_WARN("PhysicsWorld::RemoveBody: invalid body ID (0xFFFFFFFF)");
        return;
    }
    if (!m_Impl || !m_Impl->PhysicsSystem) {
        VE_ENGINE_ERROR("PhysicsWorld::RemoveBody: physics world not initialized");
        return;
    }
    auto& bodyInterface = m_Impl->PhysicsSystem->GetBodyInterface();
    JPH::BodyID id(joltBodyID);
    bodyInterface.RemoveBody(id);
    bodyInterface.DestroyBody(id);
}

// ── Raycast ─────────────────────────────────────────────────────────

bool PhysicsWorld::Raycast(const glm::vec3& origin, const glm::vec3& direction,
                           float maxDistance, RaycastHit& outHit) const {
    JPH::RRayCast ray;
    ray.mOrigin = JPH::RVec3(origin.x, origin.y, origin.z);
    glm::vec3 dir = glm::normalize(direction);
    ray.mDirection = JPH::Vec3(dir.x * maxDistance, dir.y * maxDistance, dir.z * maxDistance);

    JPH::RayCastResult result;
    auto& nq = m_Impl->PhysicsSystem->GetNarrowPhaseQuery();
    if (nq.CastRay(ray, result)) {
        JPH::Vec3 hitPoint = ray.GetPointOnRay(result.mFraction);
        outHit.Point = glm::vec3(hitPoint.GetX(), hitPoint.GetY(), hitPoint.GetZ());
        outHit.Distance = result.mFraction * maxDistance;
        outHit.BodyID = result.mBodyID.GetIndexAndSequenceNumber();

        // Get surface normal
        auto& bodyInterface = m_Impl->PhysicsSystem->GetBodyInterface();
        JPH::Vec3 normal = bodyInterface.GetShape(result.mBodyID)->
            GetSurfaceNormal(result.mSubShapeID2, hitPoint);
        outHit.Normal = glm::vec3(normal.GetX(), normal.GetY(), normal.GetZ());

        return true;
    }
    return false;
}

// ── Body manipulation ───────────────────────────────────────────────

void PhysicsWorld::AddForce(uint32_t bodyID, const glm::vec3& force) {
    if (bodyID == 0xFFFFFFFF) { VE_ENGINE_WARN("PhysicsWorld::AddForce: invalid body ID"); return; }
    if (!m_Impl || !m_Impl->PhysicsSystem) { VE_ENGINE_ERROR("PhysicsWorld::AddForce: physics world not initialized"); return; }
    auto& bi = m_Impl->PhysicsSystem->GetBodyInterface();
    bi.AddForce(JPH::BodyID(bodyID), ToJolt(force));
}

void PhysicsWorld::AddImpulse(uint32_t bodyID, const glm::vec3& impulse) {
    if (bodyID == 0xFFFFFFFF) { VE_ENGINE_WARN("PhysicsWorld::AddImpulse: invalid body ID"); return; }
    if (!m_Impl || !m_Impl->PhysicsSystem) { VE_ENGINE_ERROR("PhysicsWorld::AddImpulse: physics world not initialized"); return; }
    auto& bi = m_Impl->PhysicsSystem->GetBodyInterface();
    bi.AddImpulse(JPH::BodyID(bodyID), ToJolt(impulse));
}

void PhysicsWorld::SetLinearVelocity(uint32_t bodyID, const glm::vec3& velocity) {
    if (bodyID == 0xFFFFFFFF) { VE_ENGINE_WARN("PhysicsWorld::SetLinearVelocity: invalid body ID"); return; }
    if (!m_Impl || !m_Impl->PhysicsSystem) { VE_ENGINE_ERROR("PhysicsWorld::SetLinearVelocity: physics world not initialized"); return; }
    auto& bi = m_Impl->PhysicsSystem->GetBodyInterface();
    bi.SetLinearVelocity(JPH::BodyID(bodyID), ToJolt(velocity));
}

glm::vec3 PhysicsWorld::GetLinearVelocity(uint32_t bodyID) const {
    if (bodyID == 0xFFFFFFFF) { VE_ENGINE_WARN("PhysicsWorld::GetLinearVelocity: invalid body ID"); return glm::vec3(0.0f); }
    if (!m_Impl || !m_Impl->PhysicsSystem) { VE_ENGINE_ERROR("PhysicsWorld::GetLinearVelocity: physics world not initialized"); return glm::vec3(0.0f); }
    auto& bi = m_Impl->PhysicsSystem->GetBodyInterface();
    return FromJolt(bi.GetLinearVelocity(JPH::BodyID(bodyID)));
}

void PhysicsWorld::SetAngularVelocity(uint32_t bodyID, const glm::vec3& velocity) {
    if (bodyID == 0xFFFFFFFF) { VE_ENGINE_WARN("PhysicsWorld::SetAngularVelocity: invalid body ID"); return; }
    if (!m_Impl || !m_Impl->PhysicsSystem) { VE_ENGINE_ERROR("PhysicsWorld::SetAngularVelocity: physics world not initialized"); return; }
    auto& bi = m_Impl->PhysicsSystem->GetBodyInterface();
    bi.SetAngularVelocity(JPH::BodyID(bodyID), ToJolt(velocity));
}

// ── Collision events ────────────────────────────────────────────────

const std::vector<CollisionEvent>& PhysicsWorld::GetCollisionEvents() const {
    return m_Impl->CollisionEvents;
}

void PhysicsWorld::ClearCollisionEvents() {
    m_Impl->CollisionEvents.clear();
}

} // namespace VE
