#include "VibeEngine/Animation/Animator.h"
#include "VibeEngine/Animation/Skeleton.h"
#include "VibeEngine/Animation/AnimationClip.h"
#include "VibeEngine/Asset/MeshAsset.h"
#include "VibeEngine/Renderer/Buffer.h"
#include "VibeEngine/Core/Log.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <cstring>

namespace VE {

void Animator::SetTarget(const std::shared_ptr<MeshAsset>& mesh) {
    m_Mesh = mesh;
    if (!mesh || !mesh->IsSkinned()) return;

    int boneCount = mesh->SkeletonRef->GetBoneCount();

    // Clone bind-pose vertices as our working buffer
    m_SkinnedVertices = mesh->BindPoseVertices;
    m_BoneMatrices.resize(boneCount, glm::mat4(1.0f));
    m_LocalTransforms.resize(boneCount, glm::mat4(1.0f));
    m_GlobalTransforms.resize(boneCount, glm::mat4(1.0f));

    // Create a dynamic VAO for skinned output
    m_SkinnedVAO = VertexArray::Create();
    uint32_t byteSize = static_cast<uint32_t>(m_SkinnedVertices.size() * sizeof(float));

    // Pass nullptr to get GL_DYNAMIC_DRAW, then upload initial data
    auto vb = VertexBuffer::Create(nullptr, byteSize);
    vb->SetData(m_SkinnedVertices.data(), byteSize);
    vb->SetLayout({
        { ShaderDataType::Float3, "a_Position" },
        { ShaderDataType::Float3, "a_Normal"   },
        { ShaderDataType::Float3, "a_Color"    },
        { ShaderDataType::Float2, "a_TexCoord" },
    });
    m_SkinnedVAO->AddVertexBuffer(vb);
    m_SkinnedVAO->SetIndexBuffer(IndexBuffer::Create(
        mesh->Indices.data(), static_cast<uint32_t>(mesh->Indices.size())));

    VE_ENGINE_INFO("Animator: target set — {0} bones, {1} verts",
        mesh->SkeletonRef->GetBoneCount(),
        m_SkinnedVertices.size() / 11);
}

void Animator::SetClips(std::vector<AnimationClip> clips) {
    m_OverrideClips = std::move(clips);
}

int Animator::GetClipCount() const {
    if (!m_OverrideClips.empty()) return static_cast<int>(m_OverrideClips.size());
    if (m_Mesh) return static_cast<int>(m_Mesh->Clips.size());
    return 0;
}

void Animator::Play(int clipIndex, bool loop, float speed) {
    m_ClipIndex = clipIndex;
    m_Loop = loop;
    m_Speed = speed;
    m_CurrentTime = 0.0f;
    m_Playing = true;
}

void Animator::Stop() {
    m_Playing = false;
    m_CurrentTime = 0.0f;

    // Restore bind pose
    if (m_Mesh && !m_Mesh->BindPoseVertices.empty()) {
        m_SkinnedVertices = m_Mesh->BindPoseVertices;
        ApplySkinning(); // just re-upload bind pose
    }
}

float Animator::GetCurrentClipDuration() const {
    auto& clips = m_OverrideClips.empty() ? m_Mesh->Clips : m_OverrideClips;
    if (m_ClipIndex >= 0 && m_ClipIndex < static_cast<int>(clips.size()))
        return clips[m_ClipIndex].Duration;
    return 0.0f;
}

void Animator::Update(float deltaTime) {
    if (!m_Playing || !m_Mesh || !m_Mesh->IsSkinned()) return;
    auto& clips = m_OverrideClips.empty() ? m_Mesh->Clips : m_OverrideClips;

    // ── State Machine path ───────────────────────────────────────────
    if (m_UseStateMachine && !m_StateMachine.GetStates().empty()) {
        float clipDur = GetCurrentClipDuration();
        auto eval = m_StateMachine.Evaluate(deltaTime, m_CurrentTime, clipDur);

        if (eval.StateChanged && eval.ClipB < 0) {
            // Immediate state change (transition complete)
            m_ClipIndex = eval.ClipA;
            m_Speed = eval.SpeedA;
            m_Loop = eval.LoopA;
            m_CurrentTime = 0.0f;
            m_BlendTimeB = 0.0f;
        }

        // Update clip index from state machine
        if (!m_StateMachine.IsTransitioning()) {
            m_ClipIndex = eval.ClipA;
            m_Speed = eval.SpeedA;
            m_Loop = eval.LoopA;
        }

        if (m_ClipIndex < 0 || m_ClipIndex >= static_cast<int>(clips.size())) return;
        auto& clip = clips[m_ClipIndex];

        m_CurrentTime += deltaTime * m_Speed;
        if (m_Loop && clip.Duration > 0.0f)
            m_CurrentTime = std::fmod(m_CurrentTime, clip.Duration);
        else if (!m_Loop && m_CurrentTime >= clip.Duration)
            m_CurrentTime = clip.Duration;

        // Blending?
        if (eval.ClipB >= 0 && eval.ClipB < static_cast<int>(clips.size()) && eval.BlendT > 0.0f) {
            m_BlendTimeB += deltaTime * eval.SpeedB;
            auto& clipB = clips[eval.ClipB];
            if (eval.LoopB && clipB.Duration > 0.0f)
                m_BlendTimeB = std::fmod(m_BlendTimeB, clipB.Duration);

            SamplePoseBlended(m_CurrentTime, m_ClipIndex, m_BlendTimeB, eval.ClipB, eval.BlendT);
        } else {
            SamplePose(m_CurrentTime);
        }

        ApplySkinning();
        return;
    }

    // ── Legacy direct playback path ──────────────────────────────────
    if (m_ClipIndex < 0 || m_ClipIndex >= static_cast<int>(clips.size())) return;

    auto& clip = clips[m_ClipIndex];
    m_CurrentTime += deltaTime * m_Speed;

    if (m_Loop) {
        if (clip.Duration > 0.0f)
            m_CurrentTime = std::fmod(m_CurrentTime, clip.Duration);
    } else {
        if (m_CurrentTime >= clip.Duration) {
            m_CurrentTime = clip.Duration;
            m_Playing = false;
        }
    }

    SamplePose(m_CurrentTime);
    ApplySkinning();
}

void Animator::UpdatePose(float deltaTime) {
    if (!m_Playing || !m_Mesh || !m_Mesh->IsSkinned()) return;
    auto& clips = m_OverrideClips.empty() ? m_Mesh->Clips : m_OverrideClips;

    // State machine or legacy: advance time and sample pose (no skinning)
    if (m_UseStateMachine && !m_StateMachine.GetStates().empty()) {
        float clipDur = GetCurrentClipDuration();
        auto eval = m_StateMachine.Evaluate(deltaTime, m_CurrentTime, clipDur);

        if (eval.StateChanged && eval.ClipB < 0) {
            m_ClipIndex = eval.ClipA;
            m_Speed = eval.SpeedA;
            m_Loop = eval.LoopA;
            m_CurrentTime = 0.0f;
            m_BlendTimeB = 0.0f;
        }

        if (!m_StateMachine.IsTransitioning()) {
            m_ClipIndex = eval.ClipA;
            m_Speed = eval.SpeedA;
            m_Loop = eval.LoopA;
        }

        if (m_ClipIndex < 0 || m_ClipIndex >= static_cast<int>(clips.size())) return;
        auto& clip = clips[m_ClipIndex];

        m_CurrentTime += deltaTime * m_Speed;
        if (m_Loop && clip.Duration > 0.0f)
            m_CurrentTime = std::fmod(m_CurrentTime, clip.Duration);
        else if (!m_Loop && m_CurrentTime >= clip.Duration)
            m_CurrentTime = clip.Duration;

        if (eval.ClipB >= 0 && eval.ClipB < static_cast<int>(clips.size()) && eval.BlendT > 0.0f) {
            m_BlendTimeB += deltaTime * eval.SpeedB;
            auto& clipB = clips[eval.ClipB];
            if (eval.LoopB && clipB.Duration > 0.0f)
                m_BlendTimeB = std::fmod(m_BlendTimeB, clipB.Duration);
            SamplePoseBlended(m_CurrentTime, m_ClipIndex, m_BlendTimeB, eval.ClipB, eval.BlendT);
        } else {
            SamplePose(m_CurrentTime);
        }
    } else {
        if (m_ClipIndex < 0 || m_ClipIndex >= static_cast<int>(clips.size())) return;
        auto& clip = clips[m_ClipIndex];
        m_CurrentTime += deltaTime * m_Speed;

        if (m_Loop) {
            if (clip.Duration > 0.0f)
                m_CurrentTime = std::fmod(m_CurrentTime, clip.Duration);
        } else {
            if (m_CurrentTime >= clip.Duration) {
                m_CurrentTime = clip.Duration;
                m_Playing = false;
            }
        }
        SamplePose(m_CurrentTime);
    }

    m_PoseReady = true;
    // Note: caller must invoke FinalizeBoneMatrices() + ApplySkinning() after IK
}

void Animator::FinalizeBoneMatrices() {
    if (!m_Mesh || !m_Mesh->SkeletonRef) return;
    auto& skeleton = *m_Mesh->SkeletonRef;
    int boneCount = skeleton.GetBoneCount();

    for (int i = 0; i < boneCount; i++) {
        m_BoneMatrices[i] = m_GlobalTransforms[i] * skeleton.Bones[i].InverseBindMatrix;
    }
    m_PoseReady = false;
}

static BoneKeyframe LerpKeyframes(const BoneKeyframe& a, const BoneKeyframe& b, float t) {
    BoneKeyframe result;
    result.Time = glm::mix(a.Time, b.Time, t);
    result.Position = glm::mix(a.Position, b.Position, t);
    result.Rotation = glm::slerp(a.Rotation, b.Rotation, t);
    result.Scale = glm::mix(a.Scale, b.Scale, t);
    return result;
}

static BoneKeyframe SampleTrack(const BoneTrack& track, float time) {
    if (track.Keyframes.empty())
        return {};
    if (track.Keyframes.size() == 1)
        return track.Keyframes[0];

    // Find the two keyframes to interpolate between
    for (size_t i = 0; i + 1 < track.Keyframes.size(); i++) {
        if (time <= track.Keyframes[i + 1].Time) {
            float segLen = track.Keyframes[i + 1].Time - track.Keyframes[i].Time;
            float t = (segLen > 1e-6f) ? (time - track.Keyframes[i].Time) / segLen : 0.0f;
            return LerpKeyframes(track.Keyframes[i], track.Keyframes[i + 1], t);
        }
    }
    return track.Keyframes.back();
}

void Animator::SamplePose(float time) {
    if (!m_Mesh || !m_Mesh->SkeletonRef) return;
    auto& skeleton = *m_Mesh->SkeletonRef;
    auto& clips = m_OverrideClips.empty() ? m_Mesh->Clips : m_OverrideClips;
    auto& clip = clips[m_ClipIndex];
    int boneCount = skeleton.GetBoneCount();

    // Ensure buffers are sized
    if (static_cast<int>(m_LocalTransforms.size()) != boneCount)
        m_LocalTransforms.resize(boneCount, glm::mat4(1.0f));
    if (static_cast<int>(m_GlobalTransforms.size()) != boneCount)
        m_GlobalTransforms.resize(boneCount, glm::mat4(1.0f));

    // Compute local transforms from animation (or bind pose fallback)
    for (int i = 0; i < boneCount; i++) {
        const BoneTrack* track = clip.FindTrack(i);
        if (track && !track->Keyframes.empty()) {
            BoneKeyframe kf = SampleTrack(*track, time);
            glm::mat4 T = glm::translate(glm::mat4(1.0f), kf.Position);
            glm::mat4 R = glm::toMat4(kf.Rotation);
            glm::mat4 S = glm::scale(glm::mat4(1.0f), kf.Scale);
            m_LocalTransforms[i] = T * R * S;
        } else {
            m_LocalTransforms[i] = skeleton.Bones[i].LocalBindTransform;
        }
    }

    // Forward kinematics: compute global transforms
    for (int i = 0; i < boneCount; i++) {
        if (skeleton.Bones[i].ParentIndex >= 0)
            m_GlobalTransforms[i] = m_GlobalTransforms[skeleton.Bones[i].ParentIndex] * m_LocalTransforms[i];
        else
            m_GlobalTransforms[i] = m_LocalTransforms[i];
    }

    // Final bone matrices: global * inverseBindMatrix
    for (int i = 0; i < boneCount; i++) {
        m_BoneMatrices[i] = m_GlobalTransforms[i] * skeleton.Bones[i].InverseBindMatrix;
    }
}

void Animator::SamplePoseBlended(float timeA, int clipIdxA, float timeB, int clipIdxB, float blend) {
    if (!m_Mesh || !m_Mesh->SkeletonRef) return;
    auto& skeleton = *m_Mesh->SkeletonRef;
    auto& clips = m_OverrideClips.empty() ? m_Mesh->Clips : m_OverrideClips;
    if (clipIdxA < 0 || clipIdxA >= (int)clips.size()) return;
    if (clipIdxB < 0 || clipIdxB >= (int)clips.size()) return;

    auto& clipA = clips[clipIdxA];
    auto& clipB = clips[clipIdxB];
    int boneCount = skeleton.GetBoneCount();

    // Ensure buffers are sized
    if (static_cast<int>(m_LocalTransforms.size()) != boneCount)
        m_LocalTransforms.resize(boneCount, glm::mat4(1.0f));
    if (static_cast<int>(m_GlobalTransforms.size()) != boneCount)
        m_GlobalTransforms.resize(boneCount, glm::mat4(1.0f));

    // Sample local transforms for pose A into m_LocalTransforms
    for (int i = 0; i < boneCount; i++) {
        const BoneTrack* track = clipA.FindTrack(i);
        if (track && !track->Keyframes.empty()) {
            BoneKeyframe kf = SampleTrack(*track, timeA);
            m_LocalTransforms[i] = glm::translate(glm::mat4(1.0f), kf.Position) *
                       glm::toMat4(kf.Rotation) *
                       glm::scale(glm::mat4(1.0f), kf.Scale);
        } else {
            m_LocalTransforms[i] = skeleton.Bones[i].LocalBindTransform;
        }
    }

    // Sample local transforms for pose B (temp buffer)
    std::vector<glm::mat4> localB(boneCount);
    for (int i = 0; i < boneCount; i++) {
        const BoneTrack* track = clipB.FindTrack(i);
        if (track && !track->Keyframes.empty()) {
            BoneKeyframe kf = SampleTrack(*track, timeB);
            localB[i] = glm::translate(glm::mat4(1.0f), kf.Position) *
                       glm::toMat4(kf.Rotation) *
                       glm::scale(glm::mat4(1.0f), kf.Scale);
        } else {
            localB[i] = skeleton.Bones[i].LocalBindTransform;
        }
    }

    // Blend local transforms (column-wise lerp)
    for (int i = 0; i < boneCount; i++) {
        for (int c = 0; c < 4; c++)
            m_LocalTransforms[i][c] = glm::mix(m_LocalTransforms[i][c], localB[i][c], blend);
    }

    // Forward kinematics on blended locals → globals
    for (int i = 0; i < boneCount; i++) {
        if (skeleton.Bones[i].ParentIndex >= 0)
            m_GlobalTransforms[i] = m_GlobalTransforms[skeleton.Bones[i].ParentIndex] * m_LocalTransforms[i];
        else
            m_GlobalTransforms[i] = m_LocalTransforms[i];
    }

    // Final bone matrices: global * inverseBindMatrix
    for (int i = 0; i < boneCount; i++) {
        m_BoneMatrices[i] = m_GlobalTransforms[i] * skeleton.Bones[i].InverseBindMatrix;
    }
}

void Animator::ApplySkinning() {
    if (!m_Mesh || !m_Mesh->IsSkinned() || !m_SkinnedVAO) return;

    auto& bindVerts = m_Mesh->BindPoseVertices;
    auto& skinData = m_Mesh->SkinData;
    int vertCount = static_cast<int>(bindVerts.size() / 11);
    const int stride = 11;

    for (int v = 0; v < vertCount; v++) {
        const float* src = &bindVerts[v * stride];
        float* dst = &m_SkinnedVertices[v * stride];
        auto& sv = skinData[v];

        glm::vec3 pos(src[0], src[1], src[2]);
        glm::vec3 nrm(src[3], src[4], src[5]);

        glm::vec3 skinnedPos(0.0f);
        glm::vec3 skinnedNrm(0.0f);

        for (int j = 0; j < 4; j++) {
            float w = sv.BoneWeights[j];
            if (w < 1e-6f) continue;
            int bi = sv.BoneIndices[j];
            if (bi < 0 || bi >= static_cast<int>(m_BoneMatrices.size())) continue;

            auto& mat = m_BoneMatrices[bi];
            skinnedPos += w * glm::vec3(mat * glm::vec4(pos, 1.0f));
            skinnedNrm += w * glm::mat3(mat) * nrm;
        }

        // Normalize the normal
        float nLen = glm::length(skinnedNrm);
        if (nLen > 1e-6f) skinnedNrm /= nLen;

        dst[0] = skinnedPos.x; dst[1] = skinnedPos.y; dst[2] = skinnedPos.z;
        dst[3] = skinnedNrm.x; dst[4] = skinnedNrm.y; dst[5] = skinnedNrm.z;
        // color (6,7,8) and uv (9,10) stay the same — copy from bind pose
        dst[6] = src[6]; dst[7] = src[7]; dst[8] = src[8];
        dst[9] = src[9]; dst[10] = src[10];
    }

    // Re-upload to GPU
    auto& vbs = m_SkinnedVAO->GetVertexBuffers();
    if (!vbs.empty()) {
        vbs[0]->SetData(m_SkinnedVertices.data(),
            static_cast<uint32_t>(m_SkinnedVertices.size() * sizeof(float)));
    }
}

} // namespace VE
