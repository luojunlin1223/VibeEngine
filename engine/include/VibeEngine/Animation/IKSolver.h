/*
 * IKSolver — Inverse Kinematics solvers for skeletal animation.
 *
 * Provides two algorithms:
 * - FABRIK (Forward And Backward Reaching IK): general-purpose, works with chains
 *   of any length. Iteratively moves joints toward a target.
 * - Two-Bone IK: analytical solution for two-bone chains (arms, legs). Uses law of
 *   cosines for exact angle computation and a pole vector for bend direction.
 *
 * Additionally provides utility solvers:
 * - SolveFootPlacement: adjusts foot bone to a ground height (uses Two-Bone IK)
 * - SolveLookAt: rotates a head/spine bone to face a target point
 *
 * IK is applied after animation sampling but before skinning, allowing it to
 * blend with keyframe animation via a weight parameter.
 */
#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>

namespace VE {

class Skeleton;

// ── IK Chain Definition ──────────────────────────────────────────────

struct IKChain {
    std::vector<int> BoneIndices; // ordered root → ... → end effector
};

// ── IK Target ────────────────────────────────────────────────────────

struct IKTarget {
    int EndBoneIndex = -1;          // bone to reach target (end effector)
    int ChainLength  = 2;           // number of bones in chain (2 = two-bone IK)
    glm::vec3 TargetPosition = glm::vec3(0.0f); // world-space target
    glm::vec3 PoleVector = glm::vec3(0.0f, 0.0f, 1.0f); // bend direction hint
    float Weight = 1.0f;            // 0-1 blend with animation pose
    bool Enabled = true;

    // Optional: target entity (follow another entity's position)
    uint64_t TargetEntityUUID = 0;  // 0 = use TargetPosition
};

// ── IK Solver ────────────────────────────────────────────────────────

class IKSolver {
public:
    // Build a chain of bone indices by walking up the parent hierarchy
    // from endBone for chainLength steps.
    // Returns chain ordered root → ... → endBone.
    static IKChain BuildChain(const Skeleton& skeleton, int endBone, int chainLength);

    // FABRIK solver — works with chains of any length.
    // Modifies globalTransforms in-place. localTransforms are updated to match.
    // entityTransform is the entity's world transform (for world-to-bone-space conversion).
    static void SolveFABRIK(
        const Skeleton& skeleton,
        std::vector<glm::mat4>& localTransforms,
        std::vector<glm::mat4>& globalTransforms,
        const IKChain& chain,
        const glm::vec3& target,
        const glm::vec3& poleVector,
        float weight = 1.0f,
        int maxIterations = 10,
        float tolerance = 0.001f,
        const glm::mat4& entityTransform = glm::mat4(1.0f));

    // Two-Bone IK — analytical solution for arms/legs.
    // rootBone, midBone, endBone form a chain of exactly 2 bones.
    // poleVector controls the plane of bending.
    static void SolveTwoBone(
        const Skeleton& skeleton,
        std::vector<glm::mat4>& localTransforms,
        std::vector<glm::mat4>& globalTransforms,
        int rootBone,
        int midBone,
        int endBone,
        const glm::vec3& target,
        const glm::vec3& poleVector,
        float weight = 1.0f,
        const glm::mat4& entityTransform = glm::mat4(1.0f));

    // Foot placement — adjust foot to ground height using Two-Bone IK.
    // hipBone/kneeBone/footBone form the leg chain.
    static void SolveFootPlacement(
        const Skeleton& skeleton,
        std::vector<glm::mat4>& localTransforms,
        std::vector<glm::mat4>& globalTransforms,
        int hipBone,
        int kneeBone,
        int footBone,
        float groundHeight,
        float weight = 1.0f,
        const glm::mat4& entityTransform = glm::mat4(1.0f));

    // Look-at — rotate a head/spine bone to face a target.
    // Modifies the bone's local rotation so its forward axis points at lookTarget.
    static void SolveLookAt(
        const Skeleton& skeleton,
        std::vector<glm::mat4>& localTransforms,
        std::vector<glm::mat4>& globalTransforms,
        int headBone,
        const glm::vec3& lookTarget,
        float weight = 1.0f,
        const glm::mat4& entityTransform = glm::mat4(1.0f));

private:
    // Recompute global transforms from local transforms (forward kinematics)
    static void RecomputeGlobalTransforms(
        const Skeleton& skeleton,
        const std::vector<glm::mat4>& localTransforms,
        std::vector<glm::mat4>& globalTransforms);

    // Recompute global transforms only for a subset of bones (chain)
    static void RecomputeChainGlobalTransforms(
        const Skeleton& skeleton,
        const std::vector<glm::mat4>& localTransforms,
        std::vector<glm::mat4>& globalTransforms,
        const std::vector<int>& boneIndices);

    // Extract world position from a transform matrix
    static glm::vec3 GetPosition(const glm::mat4& m);

    // Compute bone length (distance between bone and its parent in global space)
    static float GetBoneLength(
        const std::vector<glm::mat4>& globalTransforms,
        int boneIndex,
        int parentBoneIndex);
};

} // namespace VE
