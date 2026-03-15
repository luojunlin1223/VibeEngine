#include "VibeEngine/Scene/Scene.h"
#include "VibeEngine/Scene/Entity.h"
#include "VibeEngine/Scene/Components.h"
#include "VibeEngine/Scene/MeshLibrary.h"
#include "VibeEngine/Renderer/RenderCommand.h"
#include "VibeEngine/Renderer/ShadowMap.h"
#include "VibeEngine/Scripting/ScriptEngine.h"
#include "VibeEngine/Scripting/NativeScript.h"
#include "VibeEngine/Animation/Animator.h"
#include "VibeEngine/Audio/AudioEngine.h"
#include "VibeEngine/Renderer/SpriteBatchRenderer.h"
#include "VibeEngine/Renderer/ParticleSystem.h"
#include "VibeEngine/Renderer/InstancedRenderer.h"
#include "VibeEngine/Renderer/Frustum.h"
#include "VibeEngine/Renderer/LODSystem.h"
#include "VibeEngine/UI/UIRenderer.h"
#include "VibeEngine/Asset/MeshAsset.h"
#include "VibeEngine/Asset/MeshImporter.h"
#include "VibeEngine/Asset/FBXImporter.h"
#include "VibeEngine/Core/Log.h"
#include "VibeEngine/Core/Profiler.h"

#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>
#include <random>

namespace VE {

Entity Scene::CreateEntity(const std::string& name) {
    return CreateEntityWithUUID(UUID(), name);
}

Entity Scene::CreateEntityWithUUID(UUID uuid, const std::string& name) {
    Entity entity(m_Registry.create(), this);

    // Generate a unique name if the default is used
    std::string entityName = name;
    if (name == "GameObject") {
        std::ostringstream oss;
        oss << "GameObject_" << m_EntityCounter;
        entityName = oss.str();
    }
    m_EntityCounter++;

    entity.AddComponent<IDComponent>(uuid);
    entity.AddComponent<TagComponent>(entityName);
    entity.AddComponent<TransformComponent>();
    entity.AddComponent<RelationshipComponent>();

    VE_ENGINE_INFO("Entity created: {0}", entityName);
    return entity;
}

void Scene::DestroyEntity(Entity entity) {
    if (entity.HasComponent<TagComponent>()) {
        VE_ENGINE_INFO("Entity destroyed: {0}", entity.GetComponent<TagComponent>().Tag);
    }

    // Remove from parent's children list
    RemoveParent(entity.GetHandle());

    // Recursively destroy children
    if (entity.HasComponent<RelationshipComponent>()) {
        auto children = entity.GetComponent<RelationshipComponent>().Children; // copy
        for (auto child : children) {
            if (m_Registry.valid(child))
                DestroyEntity(Entity(child, this));
        }
    }

    // Clean up Jolt body if physics is running
    if (m_PhysicsWorld && entity.HasComponent<RigidbodyComponent>()) {
        auto& rb = entity.GetComponent<RigidbodyComponent>();
        m_PhysicsWorld->RemoveBody(rb._JoltBodyID);
    }
    m_Registry.destroy(entity.GetHandle());
}

void Scene::SetParent(entt::entity child, entt::entity parent) {
    if (child == parent) return;
    if (!m_Registry.valid(child) || !m_Registry.valid(parent)) return;

    // Prevent circular: walk up from parent, if we hit child, it's circular
    entt::entity check = parent;
    while (check != entt::null) {
        if (check == child) return; // circular!
        auto& rel = m_Registry.get<RelationshipComponent>(check);
        check = rel.Parent;
    }

    RemoveParent(child);

    auto& childRel = m_Registry.get<RelationshipComponent>(child);
    auto& parentRel = m_Registry.get<RelationshipComponent>(parent);
    childRel.Parent = parent;
    parentRel.Children.push_back(child);
}

void Scene::RemoveParent(entt::entity child) {
    if (!m_Registry.valid(child)) return;
    auto& childRel = m_Registry.get<RelationshipComponent>(child);
    if (childRel.Parent == entt::null) return;

    if (m_Registry.valid(childRel.Parent)) {
        auto& parentRel = m_Registry.get<RelationshipComponent>(childRel.Parent);
        auto& vec = parentRel.Children;
        vec.erase(std::remove(vec.begin(), vec.end(), child), vec.end());
    }
    childRel.Parent = entt::null;
}

glm::mat4 Scene::GetWorldTransform(entt::entity entity) const {
    if (!m_Registry.valid(entity) || !m_Registry.all_of<TransformComponent>(entity))
        return glm::mat4(1.0f);

    auto& tc = m_Registry.get<TransformComponent>(entity);
    glm::mat4 local = glm::translate(glm::mat4(1.0f),
        glm::vec3(tc.Position[0], tc.Position[1], tc.Position[2]));
    local = glm::rotate(local, glm::radians(tc.Rotation[0]), glm::vec3(1, 0, 0));
    local = glm::rotate(local, glm::radians(tc.Rotation[1]), glm::vec3(0, 1, 0));
    local = glm::rotate(local, glm::radians(tc.Rotation[2]), glm::vec3(0, 0, 1));
    local = glm::scale(local, glm::vec3(tc.Scale[0], tc.Scale[1], tc.Scale[2]));

    if (m_Registry.all_of<RelationshipComponent>(entity)) {
        auto& rel = m_Registry.get<RelationshipComponent>(entity);
        if (rel.Parent != entt::null && m_Registry.valid(rel.Parent))
            return GetWorldTransform(rel.Parent) * local;
    }

    return local;
}

bool Scene::IsEntityActiveInHierarchy(entt::entity entity) const {
    if (!m_Registry.valid(entity)) return false;
    if (m_Registry.all_of<TagComponent>(entity)) {
        if (!m_Registry.get<TagComponent>(entity).Active)
            return false;
    }
    if (m_Registry.all_of<RelationshipComponent>(entity)) {
        auto& rel = m_Registry.get<RelationshipComponent>(entity);
        if (rel.Parent != entt::null && m_Registry.valid(rel.Parent))
            return IsEntityActiveInHierarchy(rel.Parent);
    }
    return true;
}

void Scene::OnUpdate(float deltaTime) {
    // Update entity count for profiler
    Profiler::SetEntityCount(static_cast<uint32_t>(m_Registry.storage<entt::entity>().size()));

    if (m_PhysicsRunning && m_PhysicsWorld) {
        PROFILE_SCOPE("Physics");
        static constexpr float PHYSICS_DT = 1.0f / 60.0f;
        m_PhysicsAccumulator += deltaTime;
        while (m_PhysicsAccumulator >= PHYSICS_DT) {
            m_PhysicsWorld->Step(PHYSICS_DT);
            m_PhysicsAccumulator -= PHYSICS_DT;
        }
        m_PhysicsWorld->SyncTransformsToScene(m_Registry);

        // Dispatch collision callbacks to scripts
        DispatchCollisionEvents();
    }

    // Run scripts after physics
    {
        PROFILE_SCOPE("Scripts");
        auto scriptView = m_Registry.view<ScriptComponent>();
        for (auto entity : scriptView) {
            if (!IsEntityActiveInHierarchy(entity)) continue;
            auto& sc = scriptView.get<ScriptComponent>(entity);
            if (sc._Instance)
                sc._Instance->OnUpdate(deltaTime);
        }
    }

    // Update animations after scripts
    {
        auto animView = m_Registry.view<AnimatorComponent>();
        for (auto entity : animView) {
            if (!IsEntityActiveInHierarchy(entity)) continue;
            auto& ac = animView.get<AnimatorComponent>(entity);
            if (ac._Animator)
                ac._Animator->Update(deltaTime);
        }
    }

    // Update sprite animations
    {
        auto saView = m_Registry.view<SpriteAnimatorComponent, SpriteRendererComponent>();
        for (auto entity : saView) {
            if (!IsEntityActiveInHierarchy(entity)) continue;
            auto& sa = saView.get<SpriteAnimatorComponent>(entity);
            if (!sa._Playing) continue;

            sa._Timer += deltaTime;
            float frameDuration = 1.0f / sa.FrameRate;
            while (sa._Timer >= frameDuration) {
                sa._Timer -= frameDuration;
                sa._CurrentFrame++;
                if (sa._CurrentFrame > sa.EndFrame) {
                    if (sa.Loop)
                        sa._CurrentFrame = sa.StartFrame;
                    else {
                        sa._CurrentFrame = sa.EndFrame;
                        sa._Playing = false;
                        break;
                    }
                }
            }

            // Compute UV rect from current frame
            if (sa.Columns > 0 && sa.Rows > 0) {
                int col = sa._CurrentFrame % sa.Columns;
                int row = sa._CurrentFrame / sa.Columns;
                float uW = 1.0f / static_cast<float>(sa.Columns);
                float vH = 1.0f / static_cast<float>(sa.Rows);
                auto& sr = saView.get<SpriteRendererComponent>(entity);
                sr.UVRect = { col * uW, row * vH, uW, vH };
            }
        }
    }

    // Update nav agents
    UpdateNavAgents(deltaTime);

    // Update particle systems
    OnUpdateParticles(deltaTime);
}

void Scene::StartPhysics() {
    if (m_PhysicsRunning) return;
    m_PhysicsWorld = std::make_unique<PhysicsWorld>();
    m_PhysicsWorld->SyncBodiesFromScene(m_Registry);
    m_PhysicsAccumulator = 0.0f;
    m_PhysicsRunning = true;
    VE_ENGINE_INFO("Physics simulation started");
}

void Scene::StopPhysics() {
    if (!m_PhysicsRunning) return;
    // Destroy all Jolt bodies
    auto view = m_Registry.view<RigidbodyComponent>();
    for (auto entity : view) {
        auto& rb = view.get<RigidbodyComponent>(entity);
        if (rb._JoltBodyID != 0xFFFFFFFF && m_PhysicsWorld) {
            m_PhysicsWorld->RemoveBody(rb._JoltBodyID);
            rb._JoltBodyID = 0xFFFFFFFF;
        }
    }
    m_PhysicsWorld.reset();
    m_PhysicsRunning = false;
    VE_ENGINE_INFO("Physics simulation stopped");
}

void Scene::StartScripts() {
    ScriptEngine::SetActiveScene(this);

    auto view = m_Registry.view<ScriptComponent>();
    for (auto entity : view) {
        auto& sc = view.get<ScriptComponent>(entity);
        if (sc.ClassName.empty()) continue;
        if (sc._Instance) continue; // already running

        sc._Instance = ScriptEngine::CreateInstance(sc.ClassName);
        if (sc._Instance) {
            auto& id = m_Registry.get<IDComponent>(entity);
            sc._Instance->m_EntityID = static_cast<uint64_t>(id.ID);
            // Apply stored property values before OnCreate
            ScriptEngine::ApplyPropertiesToInstance(sc._Instance, sc.ClassName, sc.Properties);
            sc._Instance->OnCreate();
        }
    }
    VE_ENGINE_INFO("Scripts started");
}

void Scene::StopScripts() {
    auto view = m_Registry.view<ScriptComponent>();
    for (auto entity : view) {
        auto& sc = view.get<ScriptComponent>(entity);
        if (sc._Instance) {
            // Read back property values before destroying
            ScriptEngine::ReadPropertiesFromInstance(sc._Instance, sc.ClassName, sc.Properties);
            sc._Instance->OnDestroy();
            ScriptEngine::DestroyInstance(sc._Instance);
            sc._Instance = nullptr;
        }
    }
    VE_ENGINE_INFO("Scripts stopped");
}

entt::entity Scene::FindEntityByBodyID(uint32_t bodyID) const {
    auto view = m_Registry.view<RigidbodyComponent>();
    for (auto entity : view) {
        auto& rb = view.get<RigidbodyComponent>(entity);
        if (rb._JoltBodyID == bodyID)
            return entity;
    }
    return entt::null;
}

void Scene::DispatchCollisionEvents() {
    if (!m_PhysicsWorld) return;

    const auto& events = m_PhysicsWorld->GetCollisionEvents();
    if (events.empty()) return;

    for (const auto& ev : events) {
        entt::entity entityA = FindEntityByBodyID(ev.BodyA);
        entt::entity entityB = FindEntityByBodyID(ev.BodyB);

        uint64_t idA = 0, idB = 0;
        if (entityA != entt::null && m_Registry.all_of<IDComponent>(entityA))
            idA = static_cast<uint64_t>(m_Registry.get<IDComponent>(entityA).ID);
        if (entityB != entt::null && m_Registry.all_of<IDComponent>(entityB))
            idB = static_cast<uint64_t>(m_Registry.get<IDComponent>(entityB).ID);

        // Dispatch to scripts on both entities
        auto dispatch = [&](entt::entity entity, uint64_t otherID, bool isEnter,
                            const glm::vec3& cp, const glm::vec3& cn) {
            if (entity == entt::null) return;
            auto* sc = m_Registry.try_get<ScriptComponent>(entity);
            if (!sc || !sc->_Instance) return;

            if (isEnter) {
                ScriptCollisionInfo info;
                info.OtherEntityID = otherID;
                info.ContactPoint[0] = cp.x; info.ContactPoint[1] = cp.y; info.ContactPoint[2] = cp.z;
                info.ContactNormal[0] = cn.x; info.ContactNormal[1] = cn.y; info.ContactNormal[2] = cn.z;
                sc->_Instance->OnCollisionEnter(info);
            } else {
                sc->_Instance->OnCollisionExit(otherID);
            }
        };

        dispatch(entityA, idB, ev.IsEnter, ev.ContactPoint, ev.ContactNormal);
        dispatch(entityB, idA, ev.IsEnter, ev.ContactPoint, -ev.ContactNormal);
    }

    m_PhysicsWorld->ClearCollisionEvents();
}

void Scene::StartAnimations() {
    auto view = m_Registry.view<AnimatorComponent, MeshRendererComponent>();
    for (auto entity : view) {
        auto& ac = view.get<AnimatorComponent>(entity);
        auto& mr = view.get<MeshRendererComponent>(entity);

        // Need a skinned MeshAsset — look it up from MeshSourcePath
        if (mr.MeshSourcePath.empty()) continue;
        auto meshAsset = MeshImporter::GetOrLoad(mr.MeshSourcePath);
        if (!meshAsset || !meshAsset->IsSkinned()) continue;

        ac._Animator = std::make_shared<Animator>();
        ac._Animator->SetTarget(meshAsset);

        // If an external animation source is specified, load clips from it
        if (!ac.AnimationSourcePath.empty() && meshAsset->SkeletonRef) {
            auto externalClips = FBXImporter::ImportAnimations(ac.AnimationSourcePath, meshAsset->SkeletonRef);
            if (!externalClips.empty())
                ac._Animator->SetClips(std::move(externalClips));
        }

        // Configure state machine if enabled
        if (ac.UseStateMachine && !ac.States.empty()) {
            auto& sm = ac._Animator->GetStateMachine();
            for (auto& s : ac.States) sm.AddState(s);
            for (auto& t : ac.Transitions) sm.AddTransition(t);
            for (auto& p : ac.Parameters) sm.AddParameter(p);
            sm.SetDefaultState(ac.DefaultState);
            sm.Reset();
            ac._Animator->SetUseStateMachine(true);
            ac._Animator->Play(ac.States[ac.DefaultState].ClipIndex,
                               ac.States[ac.DefaultState].Loop,
                               ac.States[ac.DefaultState].Speed);
        } else {
            int clipCount = ac._Animator->GetClipCount();
            if (ac.PlayOnStart && ac.ClipIndex < clipCount)
                ac._Animator->Play(ac.ClipIndex, ac.Loop, ac.Speed);
        }
    }
    VE_ENGINE_INFO("Animations started");
}

void Scene::StopAnimations() {
    auto view = m_Registry.view<AnimatorComponent>();
    for (auto entity : view) {
        auto& ac = view.get<AnimatorComponent>(entity);
        if (ac._Animator) {
            ac._Animator->Stop();
            ac._Animator.reset();
        }
    }
    VE_ENGINE_INFO("Animations stopped");
}

void Scene::StartAudio() {
    if (!AudioEngine::IsInitialized()) return;

    auto view = m_Registry.view<AudioSourceComponent>();
    for (auto entity : view) {
        auto& as = view.get<AudioSourceComponent>(entity);
        if (as.ClipPath.empty()) continue;

        if (as.PlayOnAwake) {
            as._SoundHandle = AudioEngine::Play(as.ClipPath, as.Volume, as.Pitch, as.Loop);
            if (as._SoundHandle != 0 && as.Spatial) {
                AudioEngine::SetSoundSpatial(as._SoundHandle, true);
                AudioEngine::SetSoundMinMaxDistance(as._SoundHandle, as.MinDistance, as.MaxDistance);
                if (m_Registry.all_of<TransformComponent>(entity)) {
                    auto& tc = m_Registry.get<TransformComponent>(entity);
                    AudioEngine::SetSoundPosition(as._SoundHandle, tc.Position.data());
                }
            }
        }
    }
    VE_ENGINE_INFO("Audio started");
}

void Scene::StopAudio() {
    auto view = m_Registry.view<AudioSourceComponent>();
    for (auto entity : view) {
        auto& as = view.get<AudioSourceComponent>(entity);
        if (as._SoundHandle != 0) {
            AudioEngine::Stop(as._SoundHandle);
            as._SoundHandle = 0;
        }
    }
    VE_ENGINE_INFO("Audio stopped");
}

void Scene::UpdateAudio(const float listenerPos[3], const float listenerForward[3], const float listenerUp[3]) {
    if (!AudioEngine::IsInitialized()) return;

    // Update listener position
    AudioEngine::SetListenerPosition(listenerPos, listenerForward, listenerUp);

    // Update spatial sound positions
    auto view = m_Registry.view<AudioSourceComponent, TransformComponent>();
    for (auto entity : view) {
        auto& as = view.get<AudioSourceComponent>(entity);
        if (as.Spatial && as._SoundHandle != 0) {
            auto& tc = view.get<TransformComponent>(entity);
            AudioEngine::SetSoundPosition(as._SoundHandle, tc.Position.data());
        }
    }
}

glm::mat4 Scene::ComputeCameraView(const glm::mat4& worldTransform) {
    glm::vec3 position = glm::vec3(worldTransform[3]);
    glm::mat3 rotMat   = glm::mat3(worldTransform); // upper-left 3x3 includes rotation+scale
    // Camera looks down -Z in local space (OpenGL convention)
    glm::vec3 forward = glm::normalize(rotMat * glm::vec3(0, 0, -1));
    glm::vec3 up      = glm::normalize(rotMat * glm::vec3(0, 1,  0));
    return glm::lookAt(position, position + forward, up);
}

glm::mat4 Scene::ComputeCameraProjection(int projType, float fov, float size,
                                          float nearClip, float farClip, float aspectRatio) {
    if (projType == 0) { // Perspective
        return glm::perspective(glm::radians(fov), aspectRatio, nearClip, farClip);
    } else { // Orthographic
        float halfH = size;
        float halfW = halfH * aspectRatio;
        return glm::ortho(-halfW, halfW, -halfH, halfH, nearClip, farClip);
    }
}

static glm::mat4 ComputeModelMatrix(const TransformComponent& tc) {
    glm::mat4 model = glm::translate(glm::mat4(1.0f),
        glm::vec3(tc.Position[0], tc.Position[1], tc.Position[2]));
    model = glm::rotate(model, glm::radians(tc.Rotation[0]), glm::vec3(1, 0, 0));
    model = glm::rotate(model, glm::radians(tc.Rotation[1]), glm::vec3(0, 1, 0));
    model = glm::rotate(model, glm::radians(tc.Rotation[2]), glm::vec3(0, 0, 1));
    model = glm::scale(model, glm::vec3(tc.Scale[0], tc.Scale[1], tc.Scale[2]));
    return model;
}

// ── Navigation ──────────────────────────────────────────────────────

void Scene::BakeNavGrid(float cellSize, float worldSize) {
    m_NavGrid = std::make_unique<NavGrid>(
        NavGridBuilder::BuildFromScene(*this, cellSize, worldSize));
}

void Scene::UpdateNavAgents(float deltaTime) {
    if (!m_NavGrid) return;

    auto view = m_Registry.view<TransformComponent, NavAgentComponent>();
    for (auto entity : view) {
        if (!IsEntityActiveInHierarchy(entity)) continue;
        auto& tc = view.get<TransformComponent>(entity);
        auto& nav = view.get<NavAgentComponent>(entity);

        if (!nav._HasTarget || nav._Path.empty()) continue;
        if (nav._PathIndex >= static_cast<int>(nav._Path.size())) {
            nav._HasTarget = false;
            continue;
        }

        float targetX = nav._Path[nav._PathIndex][0];
        float targetZ = nav._Path[nav._PathIndex][1];
        float dx = targetX - tc.Position[0];
        float dz = targetZ - tc.Position[2];
        float dist = std::sqrt(dx * dx + dz * dz);

        if (dist <= nav.StoppingDist) {
            nav._PathIndex++;
            if (nav._PathIndex >= static_cast<int>(nav._Path.size())) {
                nav._HasTarget = false;
            }
            continue;
        }

        float speed = nav.Speed * deltaTime;
        if (speed > dist) speed = dist;

        tc.Position[0] += (dx / dist) * speed;
        tc.Position[2] += (dz / dist) * speed;

        // Face movement direction
        tc.Rotation[1] = std::atan2(dx, dz) * 57.2958f;
    }
}

void Scene::OnRenderSky(const glm::mat4& skyViewProjection) {
    if (!m_PipelineSettings.SkyEnabled) return;

    auto skyShader = MeshLibrary::GetSkyShader();
    auto skyMesh   = MeshLibrary::GetSkySphere();
    if (!skyShader || !skyMesh) return;

    auto& sky = m_PipelineSettings;

    RenderCommand::SetDepthWrite(false);
    RenderCommand::SetDepthFunc(RendererAPI::DepthFunc::LessEqual);

    skyShader->Bind();
    skyShader->ApplyRenderState(); // Cull Front, ZWrite Off, ZTest LEqual
    skyShader->SetMat4("u_MVP", skyViewProjection);
    skyShader->SetVec3("u_TopColor",
        glm::vec3(sky.SkyTopColor[0], sky.SkyTopColor[1], sky.SkyTopColor[2]));
    skyShader->SetVec3("u_BottomColor",
        glm::vec3(sky.SkyBottomColor[0], sky.SkyBottomColor[1], sky.SkyBottomColor[2]));

    if (sky.SkyTexture) {
        sky.SkyTexture->Bind(0);
        skyShader->SetInt("u_Texture", 0);
        skyShader->SetInt("u_UseTexture", 1);
    } else {
        skyShader->SetInt("u_UseTexture", 0);
    }

    RenderCommand::DrawIndexed(skyMesh);

    // Restore default state for subsequent passes
    RenderCommand::SetDepthWrite(true);
    RenderCommand::SetDepthFunc(RendererAPI::DepthFunc::Less);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
}

void Scene::ComputeShadows(const glm::mat4& viewMatrix,
                            const glm::mat4& projMatrix,
                            float nearClip, float farClip) {
    m_ShadowsComputed = false;

    if (!m_PipelineSettings.ShadowEnabled)
        return;

    // Find directional light
    glm::vec3 lightDir = glm::normalize(glm::vec3(0.3f, 1.0f, 0.5f));
    {
        auto lightView = m_Registry.view<DirectionalLightComponent>();
        for (auto lightEntity : lightView) {
            auto& dl = lightView.get<DirectionalLightComponent>(lightEntity);
            glm::vec3 dir(dl.Direction[0], dl.Direction[1], dl.Direction[2]);
            float len = glm::length(dir);
            if (len > 0.0001f)
                lightDir = dir / len;
            break;
        }
    }

    // Create shadow map lazily
    if (!m_ShadowMap)
        m_ShadowMap = std::make_unique<ShadowMap>();

    m_CachedViewMatrix = viewMatrix;
    m_ShadowMap->ComputeCascades(viewMatrix, projMatrix, lightDir, nearClip, farClip);

    // Render depth for each cascade
    auto depthShader = m_ShadowMap->GetDepthShader();
    if (!depthShader) return;

    auto meshView = m_Registry.view<TransformComponent, MeshRendererComponent>();

    for (int c = 0; c < ShadowMap::NUM_CASCADES; ++c) {
        m_ShadowMap->BeginPass(c);
        depthShader->Bind();
        depthShader->SetMat4("u_LightSpaceMatrix", m_ShadowMap->GetLightSpaceMatrix(c));

        for (auto entityID : meshView) {
            if (!IsEntityActiveInHierarchy(entityID)) continue;
            auto [tc, mr] = meshView.get<TransformComponent, MeshRendererComponent>(entityID);
            if (!mr.Mesh || !mr.CastShadows) continue;

            std::shared_ptr<VertexArray> shadowVAO = mr.Mesh;
            if (m_Registry.all_of<AnimatorComponent>(entityID)) {
                auto& ac = m_Registry.get<AnimatorComponent>(entityID);
                if (ac._Animator && ac._Animator->GetSkinnedVAO())
                    shadowVAO = ac._Animator->GetSkinnedVAO();
            }

            glm::mat4 model = GetWorldTransform(entityID);
            depthShader->SetMat4("u_Model", model);
            RenderCommand::DrawIndexed(shadowVAO);
        }

        m_ShadowMap->EndPass();
    }

    m_ShadowsComputed = true;
}

void Scene::OnRender(const glm::mat4& viewProjection, const glm::vec3& cameraPos) {
    // Find directional light in the scene (use first one found, fallback to default)
    glm::vec3 lightDir = glm::normalize(glm::vec3(0.3f, 1.0f, 0.5f));
    glm::vec3 lightColor(1.0f);
    float lightIntensity = 1.0f;
    {
        auto lightView = m_Registry.view<DirectionalLightComponent>();
        for (auto lightEntity : lightView) {
            if (!IsEntityActiveInHierarchy(lightEntity)) continue;
            auto& dl = lightView.get<DirectionalLightComponent>(lightEntity);
            glm::vec3 dir(dl.Direction[0], dl.Direction[1], dl.Direction[2]);
            float len = glm::length(dir);
            if (len > 0.0001f)
                lightDir = dir / len;
            lightColor = glm::vec3(dl.Color[0], dl.Color[1], dl.Color[2]);
            lightIntensity = dl.Intensity;
            break; // use first directional light
        }
    }

    // Gather point lights (max 8)
    static constexpr int MAX_POINT_LIGHTS = 8;
    int numPointLights = 0;
    glm::vec3 pointPositions[MAX_POINT_LIGHTS];
    glm::vec3 pointColors[MAX_POINT_LIGHTS];
    float     pointIntensities[MAX_POINT_LIGHTS];
    float     pointRanges[MAX_POINT_LIGHTS];
    {
        auto plView = m_Registry.view<TransformComponent, PointLightComponent>();
        for (auto plEntity : plView) {
            if (numPointLights >= MAX_POINT_LIGHTS) break;
            if (!IsEntityActiveInHierarchy(plEntity)) continue;
            auto [tc, pl] = plView.get<TransformComponent, PointLightComponent>(plEntity);
            glm::mat4 worldMat = GetWorldTransform(plEntity);
            pointPositions[numPointLights]  = glm::vec3(worldMat[3]); // extract translation
            pointColors[numPointLights]     = glm::vec3(pl.Color[0], pl.Color[1], pl.Color[2]);
            pointIntensities[numPointLights] = pl.Intensity;
            pointRanges[numPointLights]     = pl.Range;
            numPointLights++;
        }
    }

    // Gather spot lights (max 4)
    static constexpr int MAX_SPOT_LIGHTS = 4;
    int numSpotLights = 0;
    glm::vec3 spotPositions[MAX_SPOT_LIGHTS];
    glm::vec3 spotDirections[MAX_SPOT_LIGHTS];
    glm::vec3 spotColors[MAX_SPOT_LIGHTS];
    float     spotIntensities[MAX_SPOT_LIGHTS];
    float     spotRanges[MAX_SPOT_LIGHTS];
    float     spotInnerCos[MAX_SPOT_LIGHTS];
    float     spotOuterCos[MAX_SPOT_LIGHTS];
    {
        auto slView = m_Registry.view<TransformComponent, SpotLightComponent>();
        for (auto slEntity : slView) {
            if (numSpotLights >= MAX_SPOT_LIGHTS) break;
            if (!IsEntityActiveInHierarchy(slEntity)) continue;
            auto [tc, sl] = slView.get<TransformComponent, SpotLightComponent>(slEntity);
            glm::mat4 worldMat = GetWorldTransform(slEntity);
            spotPositions[numSpotLights]  = glm::vec3(worldMat[3]);
            // Transform direction by world rotation (extract upper 3x3)
            glm::vec3 localDir = glm::normalize(glm::vec3(sl.Direction[0], sl.Direction[1], sl.Direction[2]));
            spotDirections[numSpotLights] = glm::normalize(glm::mat3(worldMat) * localDir);
            spotColors[numSpotLights]     = glm::vec3(sl.Color[0], sl.Color[1], sl.Color[2]);
            spotIntensities[numSpotLights] = sl.Intensity;
            spotRanges[numSpotLights]     = sl.Range;
            spotInnerCos[numSpotLights]   = std::cos(glm::radians(sl.InnerAngle));
            spotOuterCos[numSpotLights]   = std::cos(glm::radians(sl.OuterAngle));
            numSpotLights++;
        }
    }

    // Helper: set lighting uniforms on a shader (used for both individual and instanced paths)
    auto setLightingUniforms = [&](const std::shared_ptr<Shader>& shader, bool isLit) {
        if (!isLit) return;
        shader->SetVec3("u_LightDir", lightDir);
        shader->SetVec3("u_LightColor", lightColor);
        shader->SetFloat("u_LightIntensity", lightIntensity);
        shader->SetVec3("u_ViewPos", cameraPos);

        if (m_ShadowsComputed && m_ShadowMap) {
            shader->SetInt("u_ShadowEnabled", 1);
            m_ShadowMap->BindForReading(8);
            shader->SetInt("u_ShadowMap", 8);
            shader->SetFloat("u_ShadowBias", m_PipelineSettings.ShadowBias);
            shader->SetFloat("u_ShadowNormalBias", m_PipelineSettings.ShadowNormalBias);
            shader->SetInt("u_PCFRadius", m_PipelineSettings.ShadowPCFRadius);
            shader->SetMat4("u_ViewMatrix", m_CachedViewMatrix);
            for (int c = 0; c < ShadowMap::NUM_CASCADES; ++c) {
                std::string name = "u_LightSpaceMatrices[" + std::to_string(c) + "]";
                shader->SetMat4(name, m_ShadowMap->GetLightSpaceMatrix(c));
            }
            shader->SetVec3("u_CascadeSplits",
                glm::vec3(m_ShadowMap->GetCascadeSplit(0),
                          m_ShadowMap->GetCascadeSplit(1),
                          m_ShadowMap->GetCascadeSplit(2)));
        } else {
            shader->SetInt("u_ShadowEnabled", 0);
        }

        shader->SetInt("u_NumPointLights", numPointLights);
        for (int i = 0; i < numPointLights; ++i) {
            std::string idx = std::to_string(i);
            shader->SetVec3("u_PointLightPositions[" + idx + "]",  pointPositions[i]);
            shader->SetVec3("u_PointLightColors[" + idx + "]",     pointColors[i]);
            shader->SetFloat("u_PointLightIntensities[" + idx + "]", pointIntensities[i]);
            shader->SetFloat("u_PointLightRanges[" + idx + "]",    pointRanges[i]);
        }

        shader->SetInt("u_NumSpotLights", numSpotLights);
        for (int i = 0; i < numSpotLights; ++i) {
            std::string idx = std::to_string(i);
            shader->SetVec3("u_SpotLightPositions[" + idx + "]",    spotPositions[i]);
            shader->SetVec3("u_SpotLightDirections[" + idx + "]",   spotDirections[i]);
            shader->SetVec3("u_SpotLightColors[" + idx + "]",       spotColors[i]);
            shader->SetFloat("u_SpotLightIntensities[" + idx + "]", spotIntensities[i]);
            shader->SetFloat("u_SpotLightRanges[" + idx + "]",      spotRanges[i]);
            shader->SetFloat("u_SpotLightInnerCos[" + idx + "]",    spotInnerCos[i]);
            shader->SetFloat("u_SpotLightOuterCos[" + idx + "]",    spotOuterCos[i]);
        }
    };

    auto setPBRDefaults = [](const std::shared_ptr<Shader>& shader, const std::shared_ptr<Material>& mat) {
        bool hasMetallic = false, hasRoughness = false, hasAO = false;
        bool hasBumpScale = false, hasOccStr = false, hasEmission = false, hasCutoff = false;
        for (auto& prop : mat->GetProperties()) {
            if (prop.Name == "u_Metallic") hasMetallic = true;
            if (prop.Name == "u_Roughness") hasRoughness = true;
            if (prop.Name == "u_AO") hasAO = true;
            if (prop.Name == "u_BumpScale") hasBumpScale = true;
            if (prop.Name == "u_OcclusionStrength") hasOccStr = true;
            if (prop.Name == "u_EmissionColor") hasEmission = true;
            if (prop.Name == "u_Cutoff") hasCutoff = true;
        }
        if (!hasMetallic)  shader->SetFloat("u_Metallic", 0.0f);
        if (!hasRoughness) shader->SetFloat("u_Roughness", 0.5f);
        if (!hasAO)        shader->SetFloat("u_AO", 1.0f);
        if (!hasBumpScale) shader->SetFloat("u_BumpScale", 1.0f);
        if (!hasOccStr)    shader->SetFloat("u_OcclusionStrength", 1.0f);
        if (!hasEmission)  shader->SetVec4("u_EmissionColor", glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        if (!hasCutoff)    shader->SetFloat("u_Cutoff", 0.0f);
    };

    // ── Frustum culling setup ─────────────────────────────────────────
    Frustum frustum(viewProjection);
    static const AABB s_UnitAABB = { glm::vec3(-0.5f), glm::vec3(0.5f) };

    auto& stats = const_cast<RenderStats&>(RenderCommand::GetStats());

    // Helper: draw a single entity (non-instanced path)
    auto drawEntity = [&](entt::entity entityID, MeshRendererComponent& mr, const glm::mat4& model) {
        std::shared_ptr<VertexArray> drawVAO = mr.Mesh;
        bool hasAnimator = m_Registry.all_of<AnimatorComponent>(entityID) &&
                           m_Registry.get<AnimatorComponent>(entityID)._Animator &&
                           m_Registry.get<AnimatorComponent>(entityID)._Animator->GetSkinnedVAO();
        if (hasAnimator) {
            auto& ac = m_Registry.get<AnimatorComponent>(entityID);
            drawVAO = ac._Animator->GetSkinnedVAO();
        }

        glm::mat4 mvp = viewProjection * model;
        glm::vec4 entityColor(mr.Color[0], mr.Color[1], mr.Color[2], mr.Color[3]);

        mr.Mat->Bind();
        auto shader = mr.Mat->GetShader();
        if (!shader) return;

        // Ensure correct GL state for individual draws (material's ApplyRenderState
        // may set Cull Front or disable cull for some shaders — restore defaults)
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);

        shader->SetMat4("u_MVP", mvp);
        shader->SetMat4("u_Model", model); // always set model for both lit/unlit

        // Global time for animated shaders (water, etc.)
        static auto startTime = std::chrono::high_resolution_clock::now();
        auto now = std::chrono::high_resolution_clock::now();
        float globalTime = std::chrono::duration<float>(now - startTime).count();
        shader->SetFloat("u_Time", globalTime);

        bool hasTexInMat = false;
        for (auto& prop : mr.Mat->GetProperties()) {
            if (prop.Type == MaterialPropertyType::Texture2D && prop.TextureRef)
                hasTexInMat = true;
        }
        if (!hasTexInMat)
            shader->SetInt("u_UseTexture", 0);

        // Always set lighting uniforms for lit materials
        if (mr.Mat->IsLit()) {
            setLightingUniforms(shader, true);
            setPBRDefaults(shader, mr.Mat);
        }
        // Always set entity color (even if overrides exist)
        shader->SetVec4("u_EntityColor", entityColor);

        // Apply per-entity material overrides (may override EntityColor, textures, etc.)
        for (const auto& ov : mr.MaterialOverrides) {
            switch (ov.Type) {
                case MaterialPropertyType::Float:
                    shader->SetFloat(ov.Name, ov.FloatValue); break;
                case MaterialPropertyType::Int:
                    shader->SetInt(ov.Name, ov.IntValue); break;
                case MaterialPropertyType::Vec3:
                    shader->SetVec3(ov.Name, ov.Vec3Value); break;
                case MaterialPropertyType::Vec4:
                    shader->SetVec4(ov.Name, ov.Vec4Value); break;
                case MaterialPropertyType::Texture2D:
                    if (ov.TextureRef) {
                        ov.TextureRef->Bind(0);
                        shader->SetInt(ov.Name, 0);
                        shader->SetInt("u_UseTexture", 1);
                    }
                    break;
            }
        }

        RenderCommand::DrawIndexed(drawVAO);
    };

    // ── Collect visible entities, separate opaque vs transparent ─────
    struct VisibleEntity {
        entt::entity ID;
        glm::mat4    Model;
        float        DistSq; // squared distance to camera (for transparent sort)
    };
    std::vector<VisibleEntity> transparentEntities;

    // ── Begin instanced batching (opaque only) ──────────────────────
    InstancedRenderer::BeginScene(viewProjection);

    auto view = m_Registry.view<TransformComponent, MeshRendererComponent>();
    for (auto entityID : view) {
        if (!IsEntityActiveInHierarchy(entityID)) continue;
        auto [tc, mr] = view.get<TransformComponent, MeshRendererComponent>(entityID);

        if (!mr.Mesh || !mr.Mat)
            continue;

        glm::mat4 model = GetWorldTransform(entityID);

        // ── Frustum cull ──
        {
            if (!mr.LocalBounds.Valid()) {
                bool found = false;
                for (int idx = 0; idx < MeshLibrary::GetMeshCount(); ++idx) {
                    if (mr.Mesh == MeshLibrary::GetMeshByIndex(idx)) {
                        mr.LocalBounds = MeshLibrary::GetMeshAABB(idx);
                        found = true;
                        break;
                    }
                }
                if (!found)
                    mr.LocalBounds = s_UnitAABB;
            }

            const AABB& localAABB = mr.LocalBounds;
            glm::vec3 worldMin( std::numeric_limits<float>::max());
            glm::vec3 worldMax(-std::numeric_limits<float>::max());
            for (int i = 0; i < 8; ++i) {
                glm::vec3 corner(
                    (i & 1) ? localAABB.Max.x : localAABB.Min.x,
                    (i & 2) ? localAABB.Max.y : localAABB.Min.y,
                    (i & 4) ? localAABB.Max.z : localAABB.Min.z
                );
                glm::vec3 worldCorner = glm::vec3(model * glm::vec4(corner, 1.0f));
                worldMin = glm::min(worldMin, worldCorner);
                worldMax = glm::max(worldMax, worldCorner);
            }

            if (!frustum.TestAABB(worldMin, worldMax)) {
                stats.CulledObjects++;
                continue;
            }
            stats.VisibleObjects++;
        }

        // ── LOD selection ───────────────────────────────────────────
        if (m_Registry.all_of<LODGroupComponent>(entityID)) {
            auto& lodGroup = m_Registry.get<LODGroupComponent>(entityID);
            if (!lodGroup.Levels.empty()) {
                glm::vec3 worldPos = glm::vec3(model[3]);
                float dist = glm::length(worldPos - cameraPos);
                int lodIndex = SelectLOD(lodGroup, dist);

                if (lodIndex < 0) {
                    stats.CulledObjects++;
                    stats.VisibleObjects--; // was counted above
                    continue; // culled by LOD distance
                }

                lodGroup._ActiveLOD = lodIndex;
                auto& level = lodGroup.Levels[lodIndex];
                if (level.Mesh)
                    mr.Mesh = level.Mesh;
            }
        }

        // Determine transparency: material blend OR per-entity alpha < 1
        bool isTransparent = mr.Mat->IsTransparent() || mr.Color[3] < 0.999f;

        if (isTransparent) {
            // Defer transparent entities for back-to-front sorted rendering
            glm::vec3 worldPos = glm::vec3(model[3]);
            float distSq = glm::dot(worldPos - cameraPos, worldPos - cameraPos);
            transparentEntities.push_back({ entityID, model, distSq });
            continue;
        }

        // ── Opaque rendering ────────────────────────────────────────
        glm::vec4 entityColor(mr.Color[0], mr.Color[1], mr.Color[2], mr.Color[3]);

        bool hasAnimator = m_Registry.all_of<AnimatorComponent>(entityID) &&
                           m_Registry.get<AnimatorComponent>(entityID)._Animator &&
                           m_Registry.get<AnimatorComponent>(entityID)._Animator->GetSkinnedVAO();

        // Only count overrides that have actual loaded textures (not empty defaults
        // auto-populated by the inspector). Without this, saved scenes with auto-
        // populated but unused overrides would bypass the instanced path.
        bool hasEffectiveOverrides = false;
        for (const auto& ov : mr.MaterialOverrides) {
            if (ov.Type == MaterialPropertyType::Texture2D && ov.TextureRef) {
                hasEffectiveOverrides = true;
                break;
            }
        }

        if (!hasAnimator && !hasEffectiveOverrides) {
            InstancedRenderer::Submit(mr.Mesh, mr.Mat, model, entityColor);
            continue;
        }

        drawEntity(entityID, mr, model);
    }

    // ── Set lighting uniforms on instanced shaders, then flush ──────
    auto litInstShader = InstancedRenderer::GetLitInstancedShader();
    if (litInstShader) {
        litInstShader->Bind();
        setLightingUniforms(litInstShader, true);
        // PBR defaults for instanced lit
        litInstShader->SetFloat("u_Metallic", 0.0f);
        litInstShader->SetFloat("u_Roughness", 0.5f);
        litInstShader->SetFloat("u_AO", 1.0f);
        litInstShader->SetFloat("u_BumpScale", 1.0f);
        litInstShader->SetFloat("u_OcclusionStrength", 1.0f);
        litInstShader->SetVec4("u_EmissionColor", glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        litInstShader->SetFloat("u_Cutoff", 0.0f);
    }

    InstancedRenderer::EndScene();

    // ── Transparent pass: sort back-to-front, draw individually ─────
    if (!transparentEntities.empty()) {
        std::sort(transparentEntities.begin(), transparentEntities.end(),
            [](const VisibleEntity& a, const VisibleEntity& b) {
                return a.DistSq > b.DistSq; // back-to-front
            });

        for (auto& ve : transparentEntities) {
            auto& mr = m_Registry.get<MeshRendererComponent>(ve.ID);
            drawEntity(ve.ID, mr, ve.Model);
        }

        // Restore default depth write after transparent pass
        RenderCommand::SetDepthWrite(true);
    }
}

// ── Terrain Rendering ───────────────────────────────────────────────

void Scene::OnRenderTerrain(const glm::mat4& viewProjection, const glm::vec3& cameraPos) {
    auto terrainView = m_Registry.view<TransformComponent, TerrainComponent>();
    for (auto entity : terrainView) {
        if (!IsEntityActiveInHierarchy(entity)) continue;
        auto& tc = terrainView.get<TransformComponent>(entity);
        auto& terrain = terrainView.get<TerrainComponent>(entity);

        // Lazy generation
        if (terrain._NeedsRebuild || !terrain._Terrain) {
            terrain._Terrain = std::make_shared<Terrain>();
            if (!terrain.HeightmapPath.empty()) {
                terrain._Terrain->GenerateFromImage(terrain.HeightmapPath,
                    terrain.WorldSizeX, terrain.WorldSizeZ, terrain.HeightScale);
            } else {
                terrain._Terrain->GenerateProcedural(terrain.Resolution,
                    terrain.WorldSizeX, terrain.WorldSizeZ, terrain.HeightScale,
                    terrain.Octaves, terrain.Persistence, terrain.Lacunarity,
                    terrain.NoiseScale, terrain.Seed);
            }
            terrain._Mesh = terrain._Terrain->GetMesh();
            terrain._NeedsRebuild = false;

            // Load textures
            for (int i = 0; i < 4; i++) {
                if (!terrain.LayerTexturePaths[i].empty())
                    terrain._LayerTextures[i] = Texture2D::Create(terrain.LayerTexturePaths[i]);
            }
        }

        if (!terrain._Mesh) continue;

        // Load terrain shader
        static std::shared_ptr<Shader> s_TerrainShader;
        if (!s_TerrainShader) {
            s_TerrainShader = Shader::CreateFromFile("shaders/Terrain.shader");
            if (!s_TerrainShader) {
                VE_ENGINE_ERROR("Failed to load Terrain.shader");
                continue;
            }
        }

        glm::mat4 model = GetWorldTransform(entity);
        glm::mat4 mvp = viewProjection * model;

        s_TerrainShader->Bind();
        s_TerrainShader->SetMat4("u_MVP", mvp);
        s_TerrainShader->SetMat4("u_Model", model);
        s_TerrainShader->SetVec4("u_EntityColor", glm::vec4(1.0f));

        // Lighting — find directional light
        glm::vec3 lightDir = glm::normalize(glm::vec3(0.3f, 1.0f, 0.5f));
        glm::vec3 lightColor(1.0f);
        float lightIntensity = 1.0f;
        {
            auto lightView = m_Registry.view<DirectionalLightComponent>();
            for (auto le : lightView) {
                if (!IsEntityActiveInHierarchy(le)) continue;
                auto& dl = lightView.get<DirectionalLightComponent>(le);
                glm::vec3 d(dl.Direction[0], dl.Direction[1], dl.Direction[2]);
                float len = glm::length(d);
                if (len > 0.0001f) lightDir = d / len;
                lightColor = glm::vec3(dl.Color[0], dl.Color[1], dl.Color[2]);
                lightIntensity = dl.Intensity;
                break;
            }
        }
        s_TerrainShader->SetVec3("u_LightDir", lightDir);
        s_TerrainShader->SetVec3("u_LightColor", lightColor);
        s_TerrainShader->SetFloat("u_LightIntensity", lightIntensity);
        s_TerrainShader->SetVec3("u_ViewPos", cameraPos);
        s_TerrainShader->SetFloat("u_Roughness", terrain.Roughness);

        // Point lights
        static constexpr int MAX_PL = 8;
        int numPL = 0;
        auto plView = m_Registry.view<TransformComponent, PointLightComponent>();
        for (auto plE : plView) {
            if (numPL >= MAX_PL) break;
            if (!IsEntityActiveInHierarchy(plE)) continue;
            auto [ptc, pl] = plView.get<TransformComponent, PointLightComponent>(plE);
            glm::vec3 plPos = glm::vec3(GetWorldTransform(plE)[3]);
            s_TerrainShader->SetVec3("u_PointLightPositions[" + std::to_string(numPL) + "]", plPos);
            s_TerrainShader->SetVec3("u_PointLightColors[" + std::to_string(numPL) + "]",
                glm::vec3(pl.Color[0], pl.Color[1], pl.Color[2]));
            s_TerrainShader->SetFloat("u_PointLightIntensities[" + std::to_string(numPL) + "]", pl.Intensity);
            s_TerrainShader->SetFloat("u_PointLightRanges[" + std::to_string(numPL) + "]", pl.Range);
            numPL++;
        }
        s_TerrainShader->SetInt("u_NumPointLights", numPL);

        // Spot lights for terrain
        static constexpr int MAX_SL = 4;
        int numSL = 0;
        auto slView = m_Registry.view<TransformComponent, SpotLightComponent>();
        for (auto slE : slView) {
            if (numSL >= MAX_SL) break;
            if (!IsEntityActiveInHierarchy(slE)) continue;
            auto [stc, sl] = slView.get<TransformComponent, SpotLightComponent>(slE);
            glm::mat4 wm = GetWorldTransform(slE);
            glm::vec3 slPos = glm::vec3(wm[3]);
            glm::vec3 localDir = glm::normalize(glm::vec3(sl.Direction[0], sl.Direction[1], sl.Direction[2]));
            glm::vec3 slDir = glm::normalize(glm::mat3(wm) * localDir);
            s_TerrainShader->SetVec3("u_SpotLightPositions[" + std::to_string(numSL) + "]", slPos);
            s_TerrainShader->SetVec3("u_SpotLightDirections[" + std::to_string(numSL) + "]", slDir);
            s_TerrainShader->SetVec3("u_SpotLightColors[" + std::to_string(numSL) + "]",
                glm::vec3(sl.Color[0], sl.Color[1], sl.Color[2]));
            s_TerrainShader->SetFloat("u_SpotLightIntensities[" + std::to_string(numSL) + "]", sl.Intensity);
            s_TerrainShader->SetFloat("u_SpotLightRanges[" + std::to_string(numSL) + "]", sl.Range);
            s_TerrainShader->SetFloat("u_SpotLightInnerCos[" + std::to_string(numSL) + "]",
                std::cos(glm::radians(sl.InnerAngle)));
            s_TerrainShader->SetFloat("u_SpotLightOuterCos[" + std::to_string(numSL) + "]",
                std::cos(glm::radians(sl.OuterAngle)));
            numSL++;
        }
        s_TerrainShader->SetInt("u_NumSpotLights", numSL);

        // Blend heights and tiling
        s_TerrainShader->SetFloat("u_BlendHeight0", terrain.BlendHeights[0]);
        s_TerrainShader->SetFloat("u_BlendHeight1", terrain.BlendHeights[1]);
        s_TerrainShader->SetFloat("u_BlendHeight2", terrain.BlendHeights[2]);
        s_TerrainShader->SetFloat("u_HeightScale", terrain.HeightScale);
        s_TerrainShader->SetFloat("u_Tiling0", terrain.LayerTiling[0]);
        s_TerrainShader->SetFloat("u_Tiling1", terrain.LayerTiling[1]);
        s_TerrainShader->SetFloat("u_Tiling2", terrain.LayerTiling[2]);
        s_TerrainShader->SetFloat("u_Tiling3", terrain.LayerTiling[3]);

        // Bind layer textures
        for (int i = 0; i < 4; i++) {
            std::string uniformName = "u_Layer" + std::to_string(i);
            s_TerrainShader->SetInt(uniformName, i);
            if (terrain._LayerTextures[i])
                terrain._LayerTextures[i]->Bind(i);
        }

        // Shadow uniforms (if available)
        s_TerrainShader->SetInt("u_ShadowEnabled", 0);

        RenderCommand::DrawIndexed(terrain._Mesh);
    }
}

// ── Sprite Rendering ────────────────────────────────────────────────

void Scene::OnRenderSprites(const glm::mat4& viewProjection) {
    auto view = m_Registry.view<TransformComponent, SpriteRendererComponent>();

    // Collect and sort by SortingOrder, then by Z position
    struct SpriteEntry {
        entt::entity Entity;
        int SortingOrder;
        float Z;
    };
    std::vector<SpriteEntry> sprites;
    sprites.reserve(view.size_hint());

    for (auto e : view) {
        if (!IsEntityActiveInHierarchy(e)) continue;
        auto& sr = view.get<SpriteRendererComponent>(e);
        auto& tc = view.get<TransformComponent>(e);
        sprites.push_back({ e, sr.SortingOrder, tc.Position[2] });
    }

    std::sort(sprites.begin(), sprites.end(), [](const SpriteEntry& a, const SpriteEntry& b) {
        if (a.SortingOrder != b.SortingOrder) return a.SortingOrder < b.SortingOrder;
        return a.Z < b.Z;
    });

    SpriteBatchRenderer::BeginBatch(viewProjection);
    for (auto& entry : sprites) {
        auto& sr = m_Registry.get<SpriteRendererComponent>(entry.Entity);
        glm::mat4 transform = GetWorldTransform(entry.Entity);
        glm::vec4 color(sr.Color[0], sr.Color[1], sr.Color[2], sr.Color[3]);
        SpriteBatchRenderer::DrawSprite(transform, color, sr.Texture, sr.UVRect);
    }
    SpriteBatchRenderer::EndBatch();
}

// ── Sprite Animation ────────────────────────────────────────────────

void Scene::StartSpriteAnimations() {
    auto view = m_Registry.view<SpriteAnimatorComponent>();
    for (auto e : view) {
        auto& sa = view.get<SpriteAnimatorComponent>(e);
        if (sa.PlayOnStart) {
            sa._Playing = true;
            sa._CurrentFrame = sa.StartFrame;
            sa._Timer = 0.0f;
        }
    }
}

void Scene::StopSpriteAnimations() {
    auto view = m_Registry.view<SpriteAnimatorComponent>();
    for (auto e : view) {
        auto& sa = view.get<SpriteAnimatorComponent>(e);
        sa._Playing = false;
        sa._Timer = 0.0f;
        sa._CurrentFrame = sa.StartFrame;
    }
}

// ── Particle System ─────────────────────────────────────────────────

static thread_local std::mt19937 s_RNG{ std::random_device{}() };

static float RandomFloat(float min, float max) {
    std::uniform_real_distribution<float> dist(min, max);
    return dist(s_RNG);
}

void Scene::StartParticles() {
    auto view = m_Registry.view<ParticleSystemComponent>();
    for (auto e : view) {
        auto& ps = view.get<ParticleSystemComponent>(e);
        ps._Particles.resize(ps.MaxParticles);
        for (auto& p : ps._Particles)
            p.Active = false;
        ps._EmissionAccumulator = 0.0f;
        ps._Playing = ps.PlayOnStart;
        ps._EmissionStopped = false;
    }
    VE_ENGINE_INFO("Particles started");
}

void Scene::StopParticles() {
    auto view = m_Registry.view<ParticleSystemComponent>();
    for (auto e : view) {
        auto& ps = view.get<ParticleSystemComponent>(e);
        ps._Particles.clear();
        ps._EmissionAccumulator = 0.0f;
        ps._Playing = false;
        ps._EmissionStopped = false;
    }
    VE_ENGINE_INFO("Particles stopped");
}

// Helper: generate a random direction on the unit sphere
static glm::vec3 RandomOnSphere() {
    float theta = RandomFloat(0.0f, 2.0f * 3.14159265f);
    float phi = std::acos(RandomFloat(-1.0f, 1.0f));
    float sp = std::sin(phi);
    return glm::vec3(sp * std::cos(theta), sp * std::sin(theta), std::cos(phi));
}

// Helper: generate a random direction within a cone (half-angle in radians) pointing up (+Y)
static glm::vec3 RandomInCone(float halfAngleRad) {
    float theta = RandomFloat(0.0f, 2.0f * 3.14159265f);
    float cosAngle = std::cos(halfAngleRad);
    float z = RandomFloat(cosAngle, 1.0f);
    float r = std::sqrt(1.0f - z * z);
    return glm::vec3(r * std::cos(theta), z, r * std::sin(theta));
}

void Scene::OnUpdateParticles(float dt) {
    auto view = m_Registry.view<TransformComponent, ParticleSystemComponent>();
    for (auto entity : view) {
        if (!IsEntityActiveInHierarchy(entity)) continue;
        auto& tc = view.get<TransformComponent>(entity);
        auto& ps = view.get<ParticleSystemComponent>(entity);
        if (!ps._Playing) continue;

        // Resize pool if MaxParticles changed at runtime
        if ((int)ps._Particles.size() != ps.MaxParticles) {
            ps._Particles.resize(ps.MaxParticles);
            for (auto& p : ps._Particles)
                p.Active = false;
        }

        glm::vec3 gravity(ps.Gravity[0], ps.Gravity[1], ps.Gravity[2]);
        glm::vec3 emitterPos(tc.Position[0], tc.Position[1], tc.Position[2]);

        // 1) Age existing particles, apply gravity, lerp color/size
        bool anyActive = false;
        for (auto& p : ps._Particles) {
            if (!p.Active) continue;
            p.Lifetime += dt;
            if (p.Lifetime >= p.MaxLife) {
                p.Active = false;
                continue;
            }
            anyActive = true;
            p.Velocity += gravity * dt;
            p.Position += p.Velocity * dt;

            float t = p.Lifetime / p.MaxLife;
            glm::vec4 sc(ps.StartColor[0], ps.StartColor[1], ps.StartColor[2], ps.StartColor[3]);
            glm::vec4 ec(ps.EndColor[0], ps.EndColor[1], ps.EndColor[2], ps.EndColor[3]);
            p.Color = glm::mix(sc, ec, t);
            p.Size = glm::mix(ps.StartSize, ps.EndSize, t);
        }

        // Non-looping: stop emitting once we have cycled; stop playing when all dead
        if (!ps.Looping && ps._EmissionStopped) {
            if (!anyActive)
                ps._Playing = false;
            continue;
        }

        // 2) Emit new particles via accumulator
        ps._EmissionAccumulator += ps.EmissionRate * dt;
        while (ps._EmissionAccumulator >= 1.0f) {
            ps._EmissionAccumulator -= 1.0f;
            // Find inactive slot
            bool emitted = false;
            for (auto& p : ps._Particles) {
                if (p.Active) continue;

                p.Active = true;
                p.Lifetime = 0.0f;
                p.MaxLife = ps.ParticleLifetime + RandomFloat(-ps.LifetimeVariance, ps.LifetimeVariance);
                if (p.MaxLife < 0.01f) p.MaxLife = 0.01f;
                p.Size = ps.StartSize;
                p.Color = glm::vec4(ps.StartColor[0], ps.StartColor[1], ps.StartColor[2], ps.StartColor[3]);

                // Spawn position and velocity based on emitter shape
                switch (ps.Shape) {
                    case EmitterShape::Point:
                        p.Position = emitterPos;
                        p.Velocity = glm::vec3(
                            RandomFloat(ps.VelocityMin[0], ps.VelocityMax[0]),
                            RandomFloat(ps.VelocityMin[1], ps.VelocityMax[1]),
                            RandomFloat(ps.VelocityMin[2], ps.VelocityMax[2]));
                        break;

                    case EmitterShape::Sphere: {
                        // Spawn at random point on sphere surface, velocity outward
                        glm::vec3 dir = RandomOnSphere();
                        float radius = RandomFloat(0.0f, ps.ShapeRadius);
                        p.Position = emitterPos + dir * radius;
                        float speed = RandomFloat(ps.SpeedMin, ps.SpeedMax);
                        p.Velocity = dir * speed;
                        break;
                    }

                    case EmitterShape::Cone: {
                        // Spawn at emitter, velocity within cone pointing up (+Y)
                        float halfAngle = glm::radians(ps.ConeAngle);
                        glm::vec3 dir = RandomInCone(halfAngle);
                        // Offset spawn position slightly within cone base radius
                        float baseRadius = ps.ShapeRadius * RandomFloat(0.0f, 1.0f);
                        glm::vec3 offset(
                            baseRadius * std::cos(RandomFloat(0.0f, 6.28318f)),
                            0.0f,
                            baseRadius * std::sin(RandomFloat(0.0f, 6.28318f)));
                        p.Position = emitterPos + offset;
                        float speed = RandomFloat(ps.SpeedMin, ps.SpeedMax);
                        p.Velocity = dir * speed;
                        break;
                    }
                }

                emitted = true;
                break;
            }

            // If pool is full and not looping, mark emission as done
            if (!emitted && !ps.Looping) {
                ps._EmissionStopped = true;
                break;
            }
        }
    }
}

void Scene::OnRenderParticles(const glm::mat4& viewProjection, const glm::vec3& cameraPos) {
    auto view = m_Registry.view<ParticleSystemComponent>();

    bool anyActive = false;
    for (auto e : view) {
        if (!IsEntityActiveInHierarchy(e)) continue;
        auto& ps = view.get<ParticleSystemComponent>(e);
        if (!ps._Playing) continue;
        for (auto& p : ps._Particles) {
            if (p.Active) { anyActive = true; break; }
        }
        if (anyActive) break;
    }
    if (!anyActive) return;

    ParticleRenderer::BeginBatch(viewProjection);

    for (auto entity : view) {
        if (!IsEntityActiveInHierarchy(entity)) continue;
        auto& ps = view.get<ParticleSystemComponent>(entity);
        if (!ps._Playing) continue;

        // DrawParticles handles back-to-front sorting and billboard rendering
        ParticleRenderer::DrawParticles(ps._Particles, cameraPos, ps.Texture);
    }

    ParticleRenderer::EndBatch();
}

// ── Runtime UI Rendering ──────────────────────────────────────────────

void Scene::OnRenderUI(uint32_t screenWidth, uint32_t screenHeight,
                       float mouseX, float mouseY, bool mouseDown) {
    // Check if any UICanvasComponent exists
    auto canvasView = m_Registry.view<UICanvasComponent>();
    bool hasCanvas = false;
    for (auto e : canvasView) { (void)e; hasCanvas = true; break; }
    if (!hasCanvas) return;

    UIRenderer::SetMouseState(mouseX, mouseY, mouseDown);
    UIRenderer::BeginFrame(screenWidth, screenHeight);

    // Helper to compute screen position from anchor + offset
    auto ComputeScreenPos = [&](const UIRectTransformComponent& rt) -> glm::vec2 {
        float anchorX = 0.0f, anchorY = 0.0f;
        switch (rt.Anchor) {
            case UIAnchorType::TopLeft:      anchorX = 0;                      anchorY = 0; break;
            case UIAnchorType::TopCenter:    anchorX = screenWidth * 0.5f;     anchorY = 0; break;
            case UIAnchorType::TopRight:     anchorX = (float)screenWidth;     anchorY = 0; break;
            case UIAnchorType::MiddleLeft:   anchorX = 0;                      anchorY = screenHeight * 0.5f; break;
            case UIAnchorType::Center:       anchorX = screenWidth * 0.5f;     anchorY = screenHeight * 0.5f; break;
            case UIAnchorType::MiddleRight:  anchorX = (float)screenWidth;     anchorY = screenHeight * 0.5f; break;
            case UIAnchorType::BottomLeft:   anchorX = 0;                      anchorY = (float)screenHeight; break;
            case UIAnchorType::BottomCenter: anchorX = screenWidth * 0.5f;     anchorY = (float)screenHeight; break;
            case UIAnchorType::BottomRight:  anchorX = (float)screenWidth;     anchorY = (float)screenHeight; break;
        }
        float x = anchorX + rt.AnchoredPosition[0] - rt.Pivot[0] * rt.Size[0];
        float y = anchorY + rt.AnchoredPosition[1] - rt.Pivot[1] * rt.Size[1];
        return { x, y };
    };

    // First pass: update button states
    auto buttonView = m_Registry.view<UIRectTransformComponent, UIButtonComponent>();
    for (auto entity : buttonView) {
        if (!IsEntityActiveInHierarchy(entity)) continue;
        auto& rt = buttonView.get<UIRectTransformComponent>(entity);
        auto& btn = buttonView.get<UIButtonComponent>(entity);
        auto pos = ComputeScreenPos(rt);
        btn._Hovered = UIRenderer::IsMouseOver(pos.x, pos.y, rt.Size[0], rt.Size[1]);
        btn._Clicked = UIRenderer::IsMouseClicked(pos.x, pos.y, rt.Size[0], rt.Size[1]);
        btn._Pressed = btn._Hovered && mouseDown;
    }

    // Second pass: render UI elements (images, then text on top)
    // Render UIImageComponents
    auto imageView = m_Registry.view<UIRectTransformComponent, UIImageComponent>();
    for (auto entity : imageView) {
        if (!IsEntityActiveInHierarchy(entity)) continue;
        auto& rt = imageView.get<UIRectTransformComponent>(entity);
        auto& img = imageView.get<UIImageComponent>(entity);
        auto pos = ComputeScreenPos(rt);

        glm::vec4 color = { img.Color[0], img.Color[1], img.Color[2], img.Color[3] };

        // If this entity also has a button, use button color
        if (m_Registry.all_of<UIButtonComponent>(entity)) {
            auto& btn = m_Registry.get<UIButtonComponent>(entity);
            const auto& c = btn._Pressed ? btn.PressedColor :
                            btn._Hovered ? btn.HoverColor : btn.NormalColor;
            color = { c[0], c[1], c[2], c[3] };
        }

        if (img._Texture)
            UIRenderer::DrawImage(pos.x, pos.y, rt.Size[0], rt.Size[1], img._Texture, color);
        else
            UIRenderer::DrawRect(pos.x, pos.y, rt.Size[0], rt.Size[1], color);
    }

    // Render buttons without images (colored rect)
    auto btnOnlyView = m_Registry.view<UIRectTransformComponent, UIButtonComponent>();
    for (auto entity : btnOnlyView) {
        if (!IsEntityActiveInHierarchy(entity)) continue;
        if (m_Registry.all_of<UIImageComponent>(entity)) continue; // already drawn
        auto& rt = btnOnlyView.get<UIRectTransformComponent>(entity);
        auto& btn = btnOnlyView.get<UIButtonComponent>(entity);
        auto pos = ComputeScreenPos(rt);
        const auto& c = btn._Pressed ? btn.PressedColor :
                        btn._Hovered ? btn.HoverColor : btn.NormalColor;
        UIRenderer::DrawRect(pos.x, pos.y, rt.Size[0], rt.Size[1],
                             { c[0], c[1], c[2], c[3] });
    }

    // Render button labels (centered text inside button rect)
    {
        auto btnLabelView = m_Registry.view<UIRectTransformComponent, UIButtonComponent>();
        for (auto entity : btnLabelView) {
            if (!IsEntityActiveInHierarchy(entity)) continue;
            auto& rt = btnLabelView.get<UIRectTransformComponent>(entity);
            auto& btn = btnLabelView.get<UIButtonComponent>(entity);
            if (btn.Label.empty()) continue;

            auto font = FontLibrary::GetDefault();
            if (!font) continue;

            float textW = font->MeasureTextWidth(btn.Label) * (btn.FontSize / font->GetPixelHeight());
            float textH = btn.FontSize;
            auto pos = ComputeScreenPos(rt);
            float tx = pos.x + (rt.Size[0] - textW) * 0.5f;
            float ty = pos.y + (rt.Size[1] - textH) * 0.5f;
            UIRenderer::DrawText(btn.Label, tx, ty, btn.FontSize,
                                 { btn.LabelColor[0], btn.LabelColor[1], btn.LabelColor[2], btn.LabelColor[3] },
                                 font);
        }
    }

    // Render UITextComponents
    auto textView = m_Registry.view<UIRectTransformComponent, UITextComponent>();
    for (auto entity : textView) {
        if (!IsEntityActiveInHierarchy(entity)) continue;
        auto& rt = textView.get<UIRectTransformComponent>(entity);
        auto& txt = textView.get<UITextComponent>(entity);
        auto pos = ComputeScreenPos(rt);

        // Lazy-load font
        if (!txt._Font) {
            if (!txt.FontPath.empty())
                txt._Font = FontAtlas::Create(txt.FontPath, txt.FontSize);
            if (!txt._Font)
                txt._Font = FontLibrary::GetDefault();
        }

        UIRenderer::DrawText(txt.Text, pos.x, pos.y, txt.FontSize,
                             { txt.Color[0], txt.Color[1], txt.Color[2], txt.Color[3] },
                             txt._Font);
    }

    UIRenderer::EndFrame();
}

} // namespace VE
