/*
 * Animator — Per-entity animation playback with CPU skinning.
 *
 * Each Animator instance owns its own skinned vertex buffer so multiple
 * entities can share the same MeshAsset but play different animations.
 * Update() advances time and computes bone matrices via forward kinematics.
 * ApplySkinning() transforms pos+normal per vertex and re-uploads the VBO.
 */
#pragma once

#include "VibeEngine/Renderer/VertexArray.h"
#include "VibeEngine/Animation/AnimStateMachine.h"
#include <glm/glm.hpp>
#include <memory>
#include <vector>

namespace VE {

struct MeshAsset;
class AnimationClip;
class Skeleton;

class Animator {
public:
    void SetTarget(const std::shared_ptr<MeshAsset>& mesh);

    void SetClips(std::vector<AnimationClip> clips);
    void Play(int clipIndex, bool loop = true, float speed = 1.0f);
    void Stop();
    void Update(float deltaTime);
    void ApplySkinning();

    // IK support: sample pose without skinning so IK can modify bones first.
    // UpdatePose() advances time + samples bones (no skinning).
    // Call FinalizeBoneMatrices() then ApplySkinning() after IK adjustments.
    void UpdatePose(float deltaTime);
    void FinalizeBoneMatrices();
    bool IsPoseReady() const { return m_PoseReady; }

    // Access bone transforms for IK modification
    std::vector<glm::mat4>& GetLocalTransforms() { return m_LocalTransforms; }
    std::vector<glm::mat4>& GetGlobalTransforms() { return m_GlobalTransforms; }
    const std::vector<glm::mat4>& GetBoneMatrices() const { return m_BoneMatrices; }
    std::vector<glm::mat4>& GetBoneMatricesRef() { return m_BoneMatrices; }
    std::shared_ptr<MeshAsset> GetMesh() const { return m_Mesh; }

    std::shared_ptr<VertexArray> GetSkinnedVAO() const { return m_SkinnedVAO; }
    bool IsPlaying() const { return m_Playing; }
    int GetClipCount() const;

    // State machine
    AnimStateMachine& GetStateMachine() { return m_StateMachine; }
    const AnimStateMachine& GetStateMachine() const { return m_StateMachine; }
    void SetUseStateMachine(bool use) { m_UseStateMachine = use; }
    bool IsUsingStateMachine() const { return m_UseStateMachine; }

    float GetCurrentTime() const { return m_CurrentTime; }
    float GetCurrentClipDuration() const;

private:
    void SamplePose(float time);
    void SamplePoseBlended(float timeA, int clipA, float timeB, int clipB, float blend);

    std::shared_ptr<MeshAsset> m_Mesh;
    std::vector<AnimationClip> m_OverrideClips;
    std::vector<float> m_SkinnedVertices;
    std::shared_ptr<VertexArray> m_SkinnedVAO;

    std::vector<glm::mat4> m_BoneMatrices;
    std::vector<glm::mat4> m_BoneMatricesB; // for blending
    std::vector<glm::mat4> m_LocalTransforms;  // per-bone local transforms (for IK)
    std::vector<glm::mat4> m_GlobalTransforms; // per-bone global transforms (for IK)
    bool m_PoseReady = false; // true after UpdatePose, cleared after ApplySkinning

    // Playback state
    int m_ClipIndex = 0;
    float m_CurrentTime = 0.0f;
    float m_Speed = 1.0f;
    bool m_Loop = true;
    bool m_Playing = false;

    // State machine
    AnimStateMachine m_StateMachine;
    bool m_UseStateMachine = false;
    float m_BlendTimeB = 0.0f; // time in blend target clip
};

} // namespace VE
