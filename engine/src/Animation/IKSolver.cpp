/*
 * IKSolver — FABRIK and Two-Bone IK implementations.
 *
 * FABRIK iteratively adjusts joint positions via forward/backward passes,
 * then converts the resulting positions back to local rotations.
 *
 * Two-Bone IK uses the law of cosines for an exact analytical solution,
 * with a pole vector to define the bending plane.
 */
#include "VibeEngine/Animation/IKSolver.h"
#include "VibeEngine/Animation/Skeleton.h"
#include "VibeEngine/Core/Log.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <cmath>
#include <algorithm>

namespace VE {

// ── Utility ──────────────────────────────────────────────────────────

glm::vec3 IKSolver::GetPosition(const glm::mat4& m) {
    return glm::vec3(m[3]);
}

float IKSolver::GetBoneLength(
    const std::vector<glm::mat4>& globalTransforms,
    int boneIndex,
    int parentBoneIndex)
{
    return glm::distance(GetPosition(globalTransforms[boneIndex]),
                         GetPosition(globalTransforms[parentBoneIndex]));
}

void IKSolver::RecomputeGlobalTransforms(
    const Skeleton& skeleton,
    const std::vector<glm::mat4>& localTransforms,
    std::vector<glm::mat4>& globalTransforms)
{
    int boneCount = skeleton.GetBoneCount();
    for (int i = 0; i < boneCount; i++) {
        if (skeleton.Bones[i].ParentIndex >= 0)
            globalTransforms[i] = globalTransforms[skeleton.Bones[i].ParentIndex] * localTransforms[i];
        else
            globalTransforms[i] = localTransforms[i];
    }
}

void IKSolver::RecomputeChainGlobalTransforms(
    const Skeleton& skeleton,
    const std::vector<glm::mat4>& localTransforms,
    std::vector<glm::mat4>& globalTransforms,
    const std::vector<int>& boneIndices)
{
    for (int idx : boneIndices) {
        if (skeleton.Bones[idx].ParentIndex >= 0)
            globalTransforms[idx] = globalTransforms[skeleton.Bones[idx].ParentIndex] * localTransforms[idx];
        else
            globalTransforms[idx] = localTransforms[idx];
    }
}

// ── Chain Building ───────────────────────────────────────────────────

IKChain IKSolver::BuildChain(const Skeleton& skeleton, int endBone, int chainLength) {
    IKChain chain;
    if (endBone < 0 || endBone >= skeleton.GetBoneCount()) return chain;

    // Walk up the hierarchy from end effector
    int current = endBone;
    for (int i = 0; i <= chainLength; i++) {
        chain.BoneIndices.push_back(current);
        if (skeleton.Bones[current].ParentIndex < 0) break;
        if (i < chainLength)
            current = skeleton.Bones[current].ParentIndex;
    }

    // Reverse so it goes root → ... → end effector
    std::reverse(chain.BoneIndices.begin(), chain.BoneIndices.end());
    return chain;
}

// ── FABRIK Solver ────────────────────────────────────────────────────

void IKSolver::SolveFABRIK(
    const Skeleton& skeleton,
    std::vector<glm::mat4>& localTransforms,
    std::vector<glm::mat4>& globalTransforms,
    const IKChain& chain,
    const glm::vec3& target,
    const glm::vec3& poleVector,
    float weight,
    int maxIterations,
    float tolerance,
    const glm::mat4& entityTransform)
{
    if (chain.BoneIndices.size() < 2 || weight <= 0.0f) return;

    int numJoints = static_cast<int>(chain.BoneIndices.size());

    // Transform target into bone space (inverse entity transform)
    glm::mat4 invEntity = glm::inverse(entityTransform);
    glm::vec3 localTarget = glm::vec3(invEntity * glm::vec4(target, 1.0f));

    // Store original positions and bone lengths
    std::vector<glm::vec3> positions(numJoints);
    std::vector<float> lengths(numJoints - 1);

    for (int i = 0; i < numJoints; i++)
        positions[i] = GetPosition(globalTransforms[chain.BoneIndices[i]]);

    for (int i = 0; i < numJoints - 1; i++)
        lengths[i] = glm::distance(positions[i], positions[i + 1]);

    // Save original positions for blending
    std::vector<glm::vec3> originalPositions = positions;

    // Compute total chain length to check reachability
    float totalLength = 0.0f;
    for (float l : lengths) totalLength += l;

    float distToTarget = glm::distance(positions[0], localTarget);

    // If target is unreachable, stretch toward it
    if (distToTarget > totalLength) {
        glm::vec3 dir = glm::normalize(localTarget - positions[0]);
        for (int i = 1; i < numJoints; i++) {
            positions[i] = positions[i - 1] + dir * lengths[i - 1];
        }
    } else {
        // FABRIK iteration
        glm::vec3 rootPos = positions[0];

        for (int iter = 0; iter < maxIterations; iter++) {
            // Check convergence
            float endDist = glm::distance(positions[numJoints - 1], localTarget);
            if (endDist < tolerance) break;

            // Forward pass: end effector → root
            positions[numJoints - 1] = localTarget;
            for (int i = numJoints - 2; i >= 0; i--) {
                glm::vec3 dir = glm::normalize(positions[i] - positions[i + 1]);
                positions[i] = positions[i + 1] + dir * lengths[i];
            }

            // Backward pass: root → end effector
            positions[0] = rootPos;
            for (int i = 1; i < numJoints; i++) {
                glm::vec3 dir = glm::normalize(positions[i] - positions[i - 1]);
                positions[i] = positions[i - 1] + dir * lengths[i - 1];
            }
        }
    }

    // Apply pole vector constraint to middle joints
    // The pole vector defines the plane in which the chain should bend.
    glm::vec3 polePosLocal = glm::vec3(invEntity * glm::vec4(poleVector, 1.0f));
    if (numJoints >= 3) {
        for (int i = 1; i < numJoints - 1; i++) {
            glm::vec3 lineDir = glm::normalize(positions[numJoints - 1] - positions[0]);
            // Project joint onto the line from root to end
            float t = glm::dot(positions[i] - positions[0], lineDir);
            glm::vec3 projected = positions[0] + lineDir * t;

            // Direction from projected point to pole
            glm::vec3 toPole = polePosLocal - projected;
            float toPoleLen = glm::length(toPole);
            if (toPoleLen < 1e-6f) continue;

            // Direction from projected to current joint
            glm::vec3 toJoint = positions[i] - projected;
            float jointDist = glm::length(toJoint);
            if (jointDist < 1e-6f) continue;

            // Rotate the joint toward the pole vector's plane
            glm::vec3 poleDir = glm::normalize(toPole);
            positions[i] = projected + poleDir * jointDist;
        }
    }

    // Blend with original positions
    if (weight < 1.0f) {
        for (int i = 0; i < numJoints; i++) {
            positions[i] = glm::mix(originalPositions[i], positions[i], weight);
        }
    }

    // Convert positions back to local rotations.
    // For each bone in the chain, compute the rotation needed to point
    // from the bone's position toward the next bone's new position.
    for (int i = 0; i < numJoints - 1; i++) {
        int boneIdx = chain.BoneIndices[i];
        int childIdx = chain.BoneIndices[i + 1];

        // Current direction (from animation)
        glm::vec3 currentDir = glm::normalize(
            GetPosition(globalTransforms[childIdx]) - GetPosition(globalTransforms[boneIdx]));

        // Desired direction (from IK solution)
        glm::vec3 desiredDir = glm::normalize(positions[i + 1] - positions[i]);

        // Compute rotation from current to desired (in global space)
        float dotVal = glm::clamp(glm::dot(currentDir, desiredDir), -1.0f, 1.0f);
        if (dotVal > 0.9999f) continue; // Already aligned

        float angle = std::acos(dotVal);
        glm::vec3 axis = glm::cross(currentDir, desiredDir);
        float axisLen = glm::length(axis);
        if (axisLen < 1e-6f) continue;
        axis /= axisLen;

        // Convert global rotation to local space
        glm::mat4 parentGlobal = glm::mat4(1.0f);
        if (skeleton.Bones[boneIdx].ParentIndex >= 0)
            parentGlobal = globalTransforms[skeleton.Bones[boneIdx].ParentIndex];
        glm::mat3 parentRotInv = glm::transpose(glm::mat3(parentGlobal));

        glm::vec3 localAxis = parentRotInv * axis;
        glm::mat4 localRot = glm::rotate(glm::mat4(1.0f), angle, localAxis);

        // Apply to local transform
        localTransforms[boneIdx] = localTransforms[boneIdx] * localRot;

        // Recompute global transforms for the rest of the chain
        std::vector<int> remaining(chain.BoneIndices.begin() + i, chain.BoneIndices.end());
        RecomputeChainGlobalTransforms(skeleton, localTransforms, globalTransforms, remaining);
    }
}

// ── Two-Bone IK Solver ───────────────────────────────────────────────

void IKSolver::SolveTwoBone(
    const Skeleton& skeleton,
    std::vector<glm::mat4>& localTransforms,
    std::vector<glm::mat4>& globalTransforms,
    int rootBone,
    int midBone,
    int endBone,
    const glm::vec3& target,
    const glm::vec3& poleVector,
    float weight,
    const glm::mat4& entityTransform)
{
    if (rootBone < 0 || midBone < 0 || endBone < 0) return;
    if (weight <= 0.0f) return;
    int boneCount = skeleton.GetBoneCount();
    if (rootBone >= boneCount || midBone >= boneCount || endBone >= boneCount) return;

    // Transform target and pole into bone space
    glm::mat4 invEntity = glm::inverse(entityTransform);
    glm::vec3 localTarget = glm::vec3(invEntity * glm::vec4(target, 1.0f));
    glm::vec3 localPole = glm::vec3(invEntity * glm::vec4(poleVector, 1.0f));

    // Get current joint positions in bone space (global transforms, pre-entity)
    glm::vec3 posA = GetPosition(globalTransforms[rootBone]);
    glm::vec3 posB = GetPosition(globalTransforms[midBone]);
    glm::vec3 posC = GetPosition(globalTransforms[endBone]);

    // Bone lengths
    float lenAB = glm::distance(posA, posB);
    float lenBC = glm::distance(posB, posC);

    if (lenAB < 1e-6f || lenBC < 1e-6f) return;

    // Distance from root to target
    float lenAT = glm::distance(posA, localTarget);

    // Clamp distance to valid range for law of cosines
    float totalLen = lenAB + lenBC;
    float minLen = std::abs(lenAB - lenBC);
    lenAT = glm::clamp(lenAT, minLen + 0.001f, totalLen - 0.001f);

    // ── Law of cosines: compute angles ──

    // Angle at A (root): between AB and AT
    float cosAngleA = (lenAB * lenAB + lenAT * lenAT - lenBC * lenBC)
                    / (2.0f * lenAB * lenAT);
    cosAngleA = glm::clamp(cosAngleA, -1.0f, 1.0f);
    float angleA = std::acos(cosAngleA);

    // Angle at B (mid): between BA and BC
    float cosAngleB = (lenAB * lenAB + lenBC * lenBC - lenAT * lenAT)
                    / (2.0f * lenAB * lenBC);
    cosAngleB = glm::clamp(cosAngleB, -1.0f, 1.0f);
    float angleB = std::acos(cosAngleB);

    // ── Compute the bending plane using pole vector ──

    // Direction from root to target
    glm::vec3 dirAT = glm::normalize(localTarget - posA);

    // Direction from root to pole
    glm::vec3 dirAPole = localPole - posA;

    // Remove component of pole direction along AT to get perpendicular
    glm::vec3 polePerp = dirAPole - glm::dot(dirAPole, dirAT) * dirAT;
    float polePerpLen = glm::length(polePerp);
    if (polePerpLen < 1e-6f) {
        // Pole is on the line — use a fallback perpendicular
        polePerp = glm::vec3(0.0f, 1.0f, 0.0f);
        polePerp = polePerp - glm::dot(polePerp, dirAT) * dirAT;
        polePerpLen = glm::length(polePerp);
        if (polePerpLen < 1e-6f) {
            polePerp = glm::vec3(1.0f, 0.0f, 0.0f);
            polePerp = polePerp - glm::dot(polePerp, dirAT) * dirAT;
            polePerpLen = glm::length(polePerp);
        }
    }
    polePerp = glm::normalize(polePerp);

    // ── Compute new joint positions ──

    // Mid joint position: rotate from AT direction by angleA toward pole
    glm::vec3 bendAxis = glm::normalize(glm::cross(dirAT, polePerp));
    glm::mat4 rotA = glm::rotate(glm::mat4(1.0f), angleA, bendAxis);
    glm::vec3 newDirAB = glm::vec3(rotA * glm::vec4(dirAT, 0.0f));

    glm::vec3 newPosB = posA + newDirAB * lenAB;
    glm::vec3 newPosC = localTarget;

    // Blend with original positions if weight < 1
    if (weight < 1.0f) {
        newPosB = glm::mix(posB, newPosB, weight);
        newPosC = glm::mix(posC, newPosC, weight);
    }

    // ── Apply rotations to local transforms ──

    // Root bone rotation: rotate to point from A toward new B
    {
        glm::vec3 oldDir = glm::normalize(posB - posA);
        glm::vec3 newDir = glm::normalize(newPosB - posA);

        float d = glm::clamp(glm::dot(oldDir, newDir), -1.0f, 1.0f);
        if (d < 0.9999f) {
            float angle = std::acos(d);
            glm::vec3 axis = glm::cross(oldDir, newDir);
            float axLen = glm::length(axis);
            if (axLen > 1e-6f) {
                axis /= axLen;
                // Convert to local space
                glm::mat4 parentGlobal = glm::mat4(1.0f);
                if (skeleton.Bones[rootBone].ParentIndex >= 0)
                    parentGlobal = globalTransforms[skeleton.Bones[rootBone].ParentIndex];
                glm::vec3 localAxis = glm::transpose(glm::mat3(parentGlobal)) * axis;
                localTransforms[rootBone] = localTransforms[rootBone]
                    * glm::rotate(glm::mat4(1.0f), angle, localAxis);
            }
        }
    }

    // Recompute globals for mid and end
    if (skeleton.Bones[midBone].ParentIndex >= 0)
        globalTransforms[midBone] = globalTransforms[skeleton.Bones[midBone].ParentIndex] * localTransforms[midBone];
    else
        globalTransforms[midBone] = localTransforms[midBone];

    // Mid bone rotation: rotate to point from new B toward new C
    {
        glm::vec3 oldDir = glm::normalize(posC - posB);
        glm::vec3 currentB = GetPosition(globalTransforms[midBone]);
        glm::vec3 newDir = glm::normalize(newPosC - currentB);

        // Recompute oldDir based on current global (after root rotation)
        glm::vec3 currentC = GetPosition(globalTransforms[endBone]);
        // Need to recompute end bone global first
        if (skeleton.Bones[endBone].ParentIndex >= 0)
            globalTransforms[endBone] = globalTransforms[skeleton.Bones[endBone].ParentIndex] * localTransforms[endBone];
        currentC = GetPosition(globalTransforms[endBone]);
        oldDir = glm::normalize(currentC - currentB);

        float d = glm::clamp(glm::dot(oldDir, newDir), -1.0f, 1.0f);
        if (d < 0.9999f) {
            float angle = std::acos(d);
            glm::vec3 axis = glm::cross(oldDir, newDir);
            float axLen = glm::length(axis);
            if (axLen > 1e-6f) {
                axis /= axLen;
                glm::mat4 parentGlobal = globalTransforms[skeleton.Bones[midBone].ParentIndex >= 0
                    ? skeleton.Bones[midBone].ParentIndex : midBone];
                glm::vec3 localAxis = glm::transpose(glm::mat3(parentGlobal)) * axis;
                localTransforms[midBone] = localTransforms[midBone]
                    * glm::rotate(glm::mat4(1.0f), angle, localAxis);
            }
        }
    }

    // Recompute global transforms for the chain and descendants
    for (int i = 0; i < boneCount; i++) {
        if (skeleton.Bones[i].ParentIndex >= 0)
            globalTransforms[i] = globalTransforms[skeleton.Bones[i].ParentIndex] * localTransforms[i];
        else
            globalTransforms[i] = localTransforms[i];
    }
}

// ── Foot Placement ───────────────────────────────────────────────────

void IKSolver::SolveFootPlacement(
    const Skeleton& skeleton,
    std::vector<glm::mat4>& localTransforms,
    std::vector<glm::mat4>& globalTransforms,
    int hipBone,
    int kneeBone,
    int footBone,
    float groundHeight,
    float weight,
    const glm::mat4& entityTransform)
{
    if (hipBone < 0 || kneeBone < 0 || footBone < 0) return;

    // Get current foot position in world space
    glm::vec3 footWorldPos = glm::vec3(entityTransform * glm::vec4(GetPosition(globalTransforms[footBone]), 1.0f));

    // If foot is above ground, adjust target to ground height
    // If foot is below ground, also adjust
    glm::vec3 targetWorld = footWorldPos;
    targetWorld.y = groundHeight;

    // Pole vector: slightly in front of the knee (forward bending)
    glm::vec3 kneeWorldPos = glm::vec3(entityTransform * glm::vec4(GetPosition(globalTransforms[kneeBone]), 1.0f));
    glm::vec3 hipWorldPos = glm::vec3(entityTransform * glm::vec4(GetPosition(globalTransforms[hipBone]), 1.0f));

    // Default pole: offset forward from the knee in the knee-hip plane
    glm::vec3 poleWorld = kneeWorldPos + glm::vec3(0.0f, 0.0f, 1.0f);

    SolveTwoBone(skeleton, localTransforms, globalTransforms,
                 hipBone, kneeBone, footBone,
                 targetWorld, poleWorld, weight, entityTransform);
}

// ── Look-At ──────────────────────────────────────────────────────────

void IKSolver::SolveLookAt(
    const Skeleton& skeleton,
    std::vector<glm::mat4>& localTransforms,
    std::vector<glm::mat4>& globalTransforms,
    int headBone,
    const glm::vec3& lookTarget,
    float weight,
    const glm::mat4& entityTransform)
{
    if (headBone < 0 || headBone >= skeleton.GetBoneCount()) return;
    if (weight <= 0.0f) return;

    // Transform look target into bone space
    glm::mat4 invEntity = glm::inverse(entityTransform);
    glm::vec3 localLookTarget = glm::vec3(invEntity * glm::vec4(lookTarget, 1.0f));

    // Get head position and current forward direction
    glm::vec3 headPos = GetPosition(globalTransforms[headBone]);

    // The "forward" direction of the head bone (typically +Y or +Z depending on rig)
    // We use the bone's local Y axis (second column of global transform) as forward
    glm::vec3 currentForward = glm::normalize(glm::vec3(globalTransforms[headBone][1]));

    // Desired direction
    glm::vec3 desiredDir = localLookTarget - headPos;
    float dist = glm::length(desiredDir);
    if (dist < 1e-6f) return;
    desiredDir /= dist;

    // Compute rotation from current forward to desired direction
    float dotVal = glm::clamp(glm::dot(currentForward, desiredDir), -1.0f, 1.0f);
    if (dotVal > 0.9999f) return; // Already looking at target

    float angle = std::acos(dotVal) * weight;
    glm::vec3 axis = glm::cross(currentForward, desiredDir);
    float axisLen = glm::length(axis);
    if (axisLen < 1e-6f) return;
    axis /= axisLen;

    // Convert to local space rotation
    glm::mat4 parentGlobal = glm::mat4(1.0f);
    if (skeleton.Bones[headBone].ParentIndex >= 0)
        parentGlobal = globalTransforms[skeleton.Bones[headBone].ParentIndex];
    glm::vec3 localAxis = glm::transpose(glm::mat3(parentGlobal)) * axis;

    localTransforms[headBone] = localTransforms[headBone]
        * glm::rotate(glm::mat4(1.0f), angle, localAxis);

    // Recompute global transforms for head and descendants
    int boneCount = skeleton.GetBoneCount();
    for (int i = headBone; i < boneCount; i++) {
        if (skeleton.Bones[i].ParentIndex >= 0)
            globalTransforms[i] = globalTransforms[skeleton.Bones[i].ParentIndex] * localTransforms[i];
        else
            globalTransforms[i] = localTransforms[i];
    }
}

} // namespace VE
