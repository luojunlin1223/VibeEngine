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
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override {
        switch ((JPH::BroadPhaseLayer::Type)inLayer) {
            case 0: return "NON_MOVING";
            case 1: return "MOVING";
            default: return "UNKNOWN";
        }
    }
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

struct PhysicsWorld::Impl {
    std::unique_ptr<JPH::TempAllocatorImpl>     TempAllocator;
    std::unique_ptr<JPH::JobSystemThreadPool>    JobSystem;
    std::unique_ptr<JPH::PhysicsSystem>          PhysicsSystem;

    BPLayerInterfaceImpl                  BPLayerInterface;
    ObjectVsBroadPhaseLayerFilterImpl     ObjVsBPFilter;
    ObjectLayerPairFilterImpl             ObjPairFilter;
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

    VE_ENGINE_INFO("PhysicsWorld initialized (Jolt Physics)");
}

PhysicsWorld::~PhysicsWorld() {
    VE_ENGINE_INFO("PhysicsWorld destroyed");
}

void PhysicsWorld::SetGravity(float x, float y, float z) {
    m_Impl->PhysicsSystem->SetGravity(JPH::Vec3(x, y, z));
}

void PhysicsWorld::SyncBodiesFromScene(entt::registry& registry) {
    auto& bodyInterface = m_Impl->PhysicsSystem->GetBodyInterface();

    auto view = registry.view<TransformComponent, RigidbodyComponent, ColliderComponent>();
    for (auto entity : view) {
        auto& tc = view.get<TransformComponent>(entity);
        auto& rb = view.get<RigidbodyComponent>(entity);
        auto& col = view.get<ColliderComponent>(entity);

        // Already has a Jolt body
        if (rb._JoltBodyID != 0xFFFFFFFF) {
            // Update kinematic body position from transform
            if (rb.Type == BodyType::Kinematic) {
                JPH::BodyID id(rb._JoltBodyID);
                bodyInterface.SetPosition(id,
                    JPH::Vec3(tc.Position[0] + col.Offset[0],
                              tc.Position[1] + col.Offset[1],
                              tc.Position[2] + col.Offset[2]),
                    JPH::EActivation::DontActivate);
                bodyInterface.SetRotation(id,
                    EulerDegreesToJoltQuat(tc.Rotation),
                    JPH::EActivation::DontActivate);
            }
            continue;
        }

        // Create collision shape
        JPH::ShapeRefC shape;
        switch (col.Shape) {
            case ColliderShape::Box:
                shape = new JPH::BoxShape(JPH::Vec3(
                    col.Size[0] * 0.5f, col.Size[1] * 0.5f, col.Size[2] * 0.5f));
                break;
            case ColliderShape::Sphere:
                shape = new JPH::SphereShape(col.Size[0] * 0.5f);
                break;
            case ColliderShape::Capsule:
                shape = new JPH::CapsuleShape(col.Size[1] * 0.5f, col.Size[0] * 0.5f);
                break;
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
            JPH::Vec3(tc.Position[0] + col.Offset[0],
                       tc.Position[1] + col.Offset[1],
                       tc.Position[2] + col.Offset[2]),
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
        auto* col = registry.try_get<ColliderComponent>(entity);
        if (col) {
            tc.Position = { pos.GetX() - col->Offset[0],
                            pos.GetY() - col->Offset[1],
                            pos.GetZ() - col->Offset[2] };
        } else {
            tc.Position = { pos.GetX(), pos.GetY(), pos.GetZ() };
        }

        tc.Rotation = JoltQuatToEulerDegrees(rot);
    }
}

void PhysicsWorld::RemoveBody(uint32_t joltBodyID) {
    if (joltBodyID == 0xFFFFFFFF) return;
    auto& bodyInterface = m_Impl->PhysicsSystem->GetBodyInterface();
    JPH::BodyID id(joltBodyID);
    bodyInterface.RemoveBody(id);
    bodyInterface.DestroyBody(id);
}

} // namespace VE
