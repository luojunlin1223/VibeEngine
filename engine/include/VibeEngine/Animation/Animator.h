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

    std::shared_ptr<VertexArray> GetSkinnedVAO() const { return m_SkinnedVAO; }
    bool IsPlaying() const { return m_Playing; }
    int GetClipCount() const;

private:
    void SamplePose(float time);

    std::shared_ptr<MeshAsset> m_Mesh;
    std::vector<AnimationClip> m_OverrideClips; // external clips (if set, used instead of mesh clips)
    std::vector<float> m_SkinnedVertices; // working copy
    std::shared_ptr<VertexArray> m_SkinnedVAO;

    // Bone matrices (final = inverse_bind * global_transform)
    std::vector<glm::mat4> m_BoneMatrices;

    // Playback state
    int m_ClipIndex = 0;
    float m_CurrentTime = 0.0f;
    float m_Speed = 1.0f;
    bool m_Loop = true;
    bool m_Playing = false;
    float m_DebugTimer = 0.0f;
};

} // namespace VE
