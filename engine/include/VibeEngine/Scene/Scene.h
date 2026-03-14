/*
 * Scene — Owns the ECS registry and manages entity lifecycle.
 *
 * The Scene is the top-level container for all game objects.
 * It provides methods to create/destroy entities and to render
 * all entities that have a MeshRendererComponent.
 */
#pragma once

#include "VibeEngine/Core/UUID.h"
#include "VibeEngine/Renderer/Texture.h"
#include "VibeEngine/Renderer/ShadowMap.h"
#include "VibeEngine/Physics/PhysicsWorld.h"
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <string>
#include <array>
#include <vector>
#include <utility>
#include <memory>

namespace VE {

class Entity;

struct RenderPipelineSettings {
    // Sky
    bool SkyEnabled = true;
    std::array<float, 3> SkyTopColor    = { 0.4f, 0.7f, 1.0f };
    std::array<float, 3> SkyBottomColor = { 0.9f, 0.9f, 0.95f };
    std::shared_ptr<Texture2D> SkyTexture;
    std::string SkyTexturePath;

    // Bloom
    bool BloomEnabled = false;
    float BloomThreshold = 0.8f;
    float BloomIntensity = 1.0f;
    int BloomIterations = 5;

    // Vignette
    bool VignetteEnabled = false;
    float VignetteIntensity = 0.5f;
    float VignetteSmoothness = 0.5f;

    // Color Adjustments
    bool ColorAdjustEnabled = false;
    float ColorExposure = 0.0f;
    float ColorContrast = 0.0f;
    float ColorSaturation = 0.0f;
    std::array<float, 3> ColorFilter = { 1.0f, 1.0f, 1.0f };
    float ColorGamma = 1.0f;

    // Shadows / Midtones / Highlights
    bool SMHEnabled = false;
    std::array<float, 3> SMH_Shadows    = { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> SMH_Midtones   = { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> SMH_Highlights = { 1.0f, 1.0f, 1.0f };
    float SMH_ShadowStart    = 0.0f;
    float SMH_ShadowEnd      = 0.3f;
    float SMH_HighlightStart = 0.55f;
    float SMH_HighlightEnd   = 1.0f;

    // Color Curves (per-channel control points in [0,1])
    bool CurvesEnabled = false;
    std::vector<std::pair<float, float>> CurvesMaster = { {0.0f, 0.0f}, {1.0f, 1.0f} };
    std::vector<std::pair<float, float>> CurvesRed    = { {0.0f, 0.0f}, {1.0f, 1.0f} };
    std::vector<std::pair<float, float>> CurvesGreen  = { {0.0f, 0.0f}, {1.0f, 1.0f} };
    std::vector<std::pair<float, float>> CurvesBlue   = { {0.0f, 0.0f}, {1.0f, 1.0f} };

    // Tonemapping
    bool TonemapEnabled = false;
    int  TonemapMode = 2; // 0=None, 1=Reinhard, 2=ACES, 3=Uncharted2

    // Fog
    bool FogEnabled = false;
    int  FogMode    = 2;  // 0=Linear, 1=Exp, 2=Exp2
    std::array<float, 3> FogColor = { 0.7f, 0.75f, 0.8f };
    float FogDensity       = 0.02f;
    float FogStart         = 10.0f;
    float FogEnd           = 100.0f;
    float FogHeightFalloff = 0.0f;
    float FogMaxOpacity    = 1.0f;

    // SSAO
    bool SSAOEnabled = false;
    float SSAORadius    = 0.5f;
    float SSAOBias      = 0.025f;
    float SSAOIntensity = 1.0f;
    int   SSAOKernelSize = 32;

    // Anti-Aliasing
    int AAMode = 0;  // 0=None, 1=MSAA 2x, 2=MSAA 4x, 3=MSAA 8x, 4=FXAA, 5=TAA
    // FXAA params
    float FXAAEdgeThreshold    = 0.0833f;
    float FXAAEdgeThresholdMin = 0.0625f;
    float FXAASubpixelQuality  = 0.75f;
    // TAA params
    float TAABlendFactor = 0.1f;  // weight of current frame (lower = more smoothing)

    // Shadows (CSM)
    bool ShadowEnabled = true;
    float ShadowBias = 0.0005f;
    float ShadowNormalBias = 0.02f;
    int   ShadowPCFRadius = 1; // 0=hard, 1=3x3, 2=5x5
};

class Scene {
public:
    Scene() = default;
    ~Scene() = default;

    Entity CreateEntity(const std::string& name = "GameObject");
    Entity CreateEntityWithUUID(UUID uuid, const std::string& name = "GameObject");
    void DestroyEntity(Entity entity);
    void SetParent(entt::entity child, entt::entity parent);
    void RemoveParent(entt::entity child);

    // Compute world model matrix (walks parent chain)
    glm::mat4 GetWorldTransform(entt::entity entity) const;

    // Check if entity is active in hierarchy (self + all parents must be active)
    bool IsEntityActiveInHierarchy(entt::entity entity) const;

    void OnUpdate(float deltaTime = 0.0f);
    void OnRenderSky(const glm::mat4& skyViewProjection);

    // Compute shadow maps (call before OnRender each frame)
    void ComputeShadows(const glm::mat4& viewMatrix,
                        const glm::mat4& projMatrix,
                        float nearClip, float farClip);
    ShadowMap* GetShadowMap() const { return m_ShadowMap.get(); }

    void OnRender(const glm::mat4& viewProjection,
                  const glm::vec3& cameraPos = glm::vec3(0.0f));
    void OnRenderTerrain(const glm::mat4& viewProjection, const glm::vec3& cameraPos);
    void OnRenderSprites(const glm::mat4& viewProjection);
    void OnRenderUI(uint32_t screenWidth, uint32_t screenHeight,
                    float mouseX, float mouseY, bool mouseDown);

    void StartPhysics();
    void StopPhysics();
    bool IsPhysicsRunning() const { return m_PhysicsRunning; }

    void StartScripts();
    void StopScripts();

    void StartAnimations();
    void StopAnimations();

    void StartSpriteAnimations();
    void StopSpriteAnimations();

    void StartAudio();
    void StopAudio();
    void UpdateAudio(const float listenerPos[3], const float listenerForward[3], const float listenerUp[3]);

    void StartParticles();
    void StopParticles();
    void OnUpdateParticles(float dt);
    void OnRenderParticles(const glm::mat4& viewProjection, const glm::vec3& cameraPos);

    // Camera helpers — compute view/projection from CameraComponent + transform
    static glm::mat4 ComputeCameraView(const glm::mat4& worldTransform);
    static glm::mat4 ComputeCameraProjection(int projType, float fov, float size,
                                              float nearClip, float farClip, float aspectRatio);

    template<typename... Components>
    auto GetAllEntitiesWith() { return m_Registry.view<Components...>(); }

    entt::registry& GetRegistry() { return m_Registry; }

    RenderPipelineSettings& GetPipelineSettings() { return m_PipelineSettings; }
    const RenderPipelineSettings& GetPipelineSettings() const { return m_PipelineSettings; }

    // Physics queries (delegates to PhysicsWorld)
    PhysicsWorld* GetPhysicsWorld() const { return m_PhysicsWorld.get(); }

    // Find entity by JoltBodyID (for collision callback dispatch)
    entt::entity FindEntityByBodyID(uint32_t bodyID) const;

    // Dispatch collision events to scripts
    void DispatchCollisionEvents();

    // Scene loading request (processed at end of frame)
    void RequestLoadScene(const std::string& path) { m_PendingScenePath = path; }
    const std::string& GetPendingScene() const { return m_PendingScenePath; }
    void ClearPendingScene() { m_PendingScenePath.clear(); }

private:
    entt::registry m_Registry;
    uint32_t m_EntityCounter = 0;
    RenderPipelineSettings m_PipelineSettings;

    std::unique_ptr<PhysicsWorld> m_PhysicsWorld;
    bool  m_PhysicsRunning = false;
    float m_PhysicsAccumulator = 0.0f;

    std::unique_ptr<ShadowMap> m_ShadowMap;
    bool m_ShadowsComputed = false; // true if ComputeShadows ran this frame
    glm::mat4 m_CachedViewMatrix = glm::mat4(1.0f); // stored from ComputeShadows for cascade selection

    std::string m_PendingScenePath; // scene to load at end of frame

    friend class Entity;
};

} // namespace VE
