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
#include "VibeEngine/Renderer/InstancedRenderer.h"
#include "VibeEngine/Renderer/Frustum.h"
#include "VibeEngine/Asset/MeshAsset.h"
#include "VibeEngine/Asset/MeshImporter.h"
#include "VibeEngine/Asset/FBXImporter.h"
#include "VibeEngine/Core/Log.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
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

void Scene::OnUpdate(float deltaTime) {
    if (m_PhysicsRunning && m_PhysicsWorld) {
        static constexpr float PHYSICS_DT = 1.0f / 60.0f;
        m_PhysicsAccumulator += deltaTime;
        while (m_PhysicsAccumulator >= PHYSICS_DT) {
            m_PhysicsWorld->Step(PHYSICS_DT);
            m_PhysicsAccumulator -= PHYSICS_DT;
        }
        m_PhysicsWorld->SyncTransformsToScene(m_Registry);
    }

    // Run scripts after physics
    {
        auto scriptView = m_Registry.view<ScriptComponent>();
        for (auto entity : scriptView) {
            auto& sc = scriptView.get<ScriptComponent>(entity);
            if (sc._Instance)
                sc._Instance->OnUpdate(deltaTime);
        }
    }

    // Update animations after scripts
    {
        auto animView = m_Registry.view<AnimatorComponent>();
        for (auto entity : animView) {
            auto& ac = animView.get<AnimatorComponent>(entity);
            if (ac._Animator)
                ac._Animator->Update(deltaTime);
        }
    }

    // Update sprite animations
    {
        auto saView = m_Registry.view<SpriteAnimatorComponent, SpriteRendererComponent>();
        for (auto entity : saView) {
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

        int clipCount = ac._Animator->GetClipCount();
        if (ac.PlayOnStart && ac.ClipIndex < clipCount)
            ac._Animator->Play(ac.ClipIndex, ac.Loop, ac.Speed);
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

void Scene::OnRenderSky(const glm::mat4& skyViewProjection) {
    if (!m_PipelineSettings.SkyEnabled) return;

    auto skyShader = MeshLibrary::GetSkyShader();
    auto skyMesh   = MeshLibrary::GetSphere();
    if (!skyShader || !skyMesh) return;

    auto& sky = m_PipelineSettings;

    RenderCommand::SetDepthWrite(false);
    RenderCommand::SetDepthFunc(RendererAPI::DepthFunc::LessEqual);

    skyShader->Bind();
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

    RenderCommand::SetDepthWrite(true);
    RenderCommand::SetDepthFunc(RendererAPI::DepthFunc::Less);
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
            auto [tc, pl] = plView.get<TransformComponent, PointLightComponent>(plEntity);
            glm::mat4 worldMat = GetWorldTransform(plEntity);
            pointPositions[numPointLights]  = glm::vec3(worldMat[3]); // extract translation
            pointColors[numPointLights]     = glm::vec3(pl.Color[0], pl.Color[1], pl.Color[2]);
            pointIntensities[numPointLights] = pl.Intensity;
            pointRanges[numPointLights]     = pl.Range;
            numPointLights++;
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

        shader->SetMat4("u_MVP", mvp);

        bool hasTexInMat = false;
        for (auto& prop : mr.Mat->GetProperties()) {
            if (prop.Type == MaterialPropertyType::Texture2D && prop.TextureRef)
                hasTexInMat = true;
        }
        if (!hasTexInMat)
            shader->SetInt("u_UseTexture", 0);

        if (mr.Mat->IsLit()) {
            shader->SetMat4("u_Model", model);
            setLightingUniforms(shader, true);
            setPBRDefaults(shader, mr.Mat);
        }

        bool hasColorOverride = false;
        for (const auto& ov : mr.MaterialOverrides) {
            if (ov.Name == "u_EntityColor") { hasColorOverride = true; break; }
        }
        if (!hasColorOverride)
            shader->SetVec4("u_EntityColor", entityColor);

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
        bool hasOverrides = !mr.MaterialOverrides.empty();

        if (!hasAnimator && !hasOverrides) {
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
    }
    VE_ENGINE_INFO("Particles stopped");
}

void Scene::OnUpdateParticles(float dt) {
    auto view = m_Registry.view<TransformComponent, ParticleSystemComponent>();
    for (auto entity : view) {
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
        for (auto& p : ps._Particles) {
            if (!p.Active) continue;
            p.Lifetime += dt;
            if (p.Lifetime >= p.MaxLife) {
                p.Active = false;
                continue;
            }
            p.Velocity += gravity * dt;
            p.Position += p.Velocity * dt;

            float t = p.Lifetime / p.MaxLife;
            glm::vec4 sc(ps.StartColor[0], ps.StartColor[1], ps.StartColor[2], ps.StartColor[3]);
            glm::vec4 ec(ps.EndColor[0], ps.EndColor[1], ps.EndColor[2], ps.EndColor[3]);
            p.Color = glm::mix(sc, ec, t);
            p.Size = glm::mix(ps.StartSize, ps.EndSize, t);
        }

        // 2) Emit new particles via accumulator
        ps._EmissionAccumulator += ps.EmissionRate * dt;
        while (ps._EmissionAccumulator >= 1.0f) {
            ps._EmissionAccumulator -= 1.0f;
            // Find inactive slot
            for (auto& p : ps._Particles) {
                if (p.Active) continue;
                p.Active = true;
                p.Position = emitterPos;
                p.Velocity = glm::vec3(
                    RandomFloat(ps.VelocityMin[0], ps.VelocityMax[0]),
                    RandomFloat(ps.VelocityMin[1], ps.VelocityMax[1]),
                    RandomFloat(ps.VelocityMin[2], ps.VelocityMax[2]));
                p.MaxLife = ps.ParticleLifetime + RandomFloat(-ps.LifetimeVariance, ps.LifetimeVariance);
                if (p.MaxLife < 0.01f) p.MaxLife = 0.01f;
                p.Lifetime = 0.0f;
                p.Size = ps.StartSize;
                p.Color = glm::vec4(ps.StartColor[0], ps.StartColor[1], ps.StartColor[2], ps.StartColor[3]);
                break;
            }
        }
    }
}

void Scene::OnRenderParticles(const glm::mat4& viewProjection, const glm::vec3& cameraPos) {
    auto view = m_Registry.view<ParticleSystemComponent>();

    bool anyActive = false;
    for (auto e : view) {
        auto& ps = view.get<ParticleSystemComponent>(e);
        if (!ps._Playing) continue;
        for (auto& p : ps._Particles) {
            if (p.Active) { anyActive = true; break; }
        }
        if (anyActive) break;
    }
    if (!anyActive) return;

    SpriteBatchRenderer::BeginBatch(viewProjection);

    for (auto entity : view) {
        auto& ps = view.get<ParticleSystemComponent>(entity);
        if (!ps._Playing) continue;

        for (auto& p : ps._Particles) {
            if (!p.Active) continue;

            // Billboard: face camera
            glm::vec3 toCamera = cameraPos - p.Position;
            float dist = glm::length(toCamera);
            if (dist < 0.0001f)
                toCamera = glm::vec3(0.0f, 0.0f, 1.0f);
            else
                toCamera /= dist;

            glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
            // Fallback when particle is directly above/below camera
            if (std::abs(glm::dot(toCamera, worldUp)) > 0.999f)
                worldUp = glm::vec3(0.0f, 0.0f, 1.0f);

            glm::vec3 right = glm::normalize(glm::cross(worldUp, toCamera));
            glm::vec3 up    = glm::cross(toCamera, right);

            float s = p.Size;
            glm::mat4 transform(1.0f);
            transform[0] = glm::vec4(right * s, 0.0f);
            transform[1] = glm::vec4(up * s, 0.0f);
            transform[2] = glm::vec4(toCamera * s, 0.0f);
            transform[3] = glm::vec4(p.Position, 1.0f);

            std::array<float, 4> uvRect = { 0.0f, 0.0f, 1.0f, 1.0f };
            SpriteBatchRenderer::DrawSprite(transform, p.Color, ps.Texture, uvRect);
        }
    }

    SpriteBatchRenderer::EndBatch();
}

} // namespace VE
