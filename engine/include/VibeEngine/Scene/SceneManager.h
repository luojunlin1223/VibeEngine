/*
 * SceneManager — Manages multiple loaded scenes with additive loading
 * and smooth transitions (fade, crossfade).
 *
 * Each loaded scene has its own ECS registry. The "active" scene is
 * the primary target for new entity creation and physics simulation.
 * Additive scenes layer on top and can be individually unloaded.
 *
 * Scene transitions run over multiple frames using delta-time-based
 * fade progress, rendered as a fullscreen overlay quad.
 */
#pragma once

#include "Scene.h"
#include "SceneSerializer.h"
#include <memory>
#include <string>
#include <vector>
#include <functional>

namespace VE {

// How to load a scene
enum class SceneLoadMode {
    Single,   // Replace all current scenes
    Additive  // Add on top of existing scenes
};

// Visual transition type when switching scenes
enum class TransitionType {
    None,       // Instant switch
    Fade,       // Fade to black, then fade in
    CrossFade   // Old scene fades out while new scene fades in
};

// Internal state of a scene transition
enum class TransitionPhase {
    None,       // No transition in progress
    FadeOut,    // Fading out old scene(s)
    Loading,    // Loading new scene (single frame)
    FadeIn      // Fading in new scene
};

// Info about a loaded scene in the manager
struct LoadedSceneInfo {
    std::shared_ptr<Scene> ScenePtr;
    std::string Name;           // Display name (from filename or user-set)
    std::string FilePath;       // Source .vscene file path (empty if unsaved)
    bool PhysicsEnabled = true; // Whether physics runs on this scene
};

class SceneManager {
public:
    SceneManager() = default;
    ~SceneManager() = default;

    // ── Scene loading ───────────────────────────────────────────────

    // Load a scene from disk. In Single mode, replaces all scenes.
    // In Additive mode, adds it alongside existing scenes.
    // Returns the index of the loaded scene, or -1 on failure.
    int LoadScene(const std::string& path, SceneLoadMode mode = SceneLoadMode::Single);

    // Add an already-created scene (e.g., a fresh empty scene).
    // Returns the index of the added scene.
    int AddScene(std::shared_ptr<Scene> scene, const std::string& name = "Untitled",
                 const std::string& filePath = "");

    // Unload a scene by index. Cannot unload if it is the only scene.
    bool UnloadScene(int index);

    // Unload all scenes
    void UnloadAllScenes();

    // ── Active scene ────────────────────────────────────────────────

    // The active scene is the primary target for entity creation, physics, etc.
    std::shared_ptr<Scene> GetActiveScene() const;
    int GetActiveSceneIndex() const { return m_ActiveSceneIndex; }
    void SetActiveScene(int index);

    // ── Scene queries ───────────────────────────────────────────────

    int GetSceneCount() const { return static_cast<int>(m_LoadedScenes.size()); }
    const LoadedSceneInfo& GetSceneInfo(int index) const { return m_LoadedScenes[index]; }
    LoadedSceneInfo& GetSceneInfo(int index) { return m_LoadedScenes[index]; }
    const std::vector<LoadedSceneInfo>& GetAllScenes() const { return m_LoadedScenes; }

    // Get active scene name (for script API)
    std::string GetActiveSceneName() const;

    // ── Update / Render (called each frame) ─────────────────────────

    // Update all loaded scenes (physics only on active or physics-enabled scenes)
    void OnUpdateAll(float deltaTime);

    // Render all loaded scenes in order (additive scenes layer on top)
    void OnRenderAllSky(const glm::mat4& skyViewProjection);
    void OnRenderAll(const glm::mat4& viewProjection, const glm::vec3& cameraPos,
                     uint32_t viewportWidth = 1280, uint32_t viewportHeight = 720);
    void OnRenderAllTerrain(const glm::mat4& viewProjection, const glm::vec3& cameraPos);
    void OnRenderAllSprites(const glm::mat4& viewProjection);
    void OnRenderAllParticles(const glm::mat4& viewProjection, const glm::vec3& cameraPos);
    void OnRenderAllUI(uint32_t screenWidth, uint32_t screenHeight,
                       float mouseX, float mouseY, bool mouseDown);

    // ── Transitions ─────────────────────────────────────────────────

    // Start a transition to a new scene. The old scene(s) fade out,
    // the new scene loads, then fades in. Non-blocking.
    void TransitionToScene(const std::string& path,
                           TransitionType type = TransitionType::Fade,
                           float duration = 1.0f);

    // Update the transition state. Call each frame.
    void UpdateTransition(float deltaTime);

    // Returns the current overlay alpha [0,1] for rendering the transition.
    // 0 = fully transparent (no overlay), 1 = fully opaque (black screen).
    float GetTransitionOverlayAlpha() const { return m_TransitionAlpha; }

    // True if a transition is currently in progress
    bool IsTransitioning() const { return m_TransitionPhase != TransitionPhase::None; }

    TransitionPhase GetTransitionPhase() const { return m_TransitionPhase; }

    // ── Play mode helpers ───────────────────────────────────────────

    // Start/stop physics, scripts, audio, etc. on all loaded scenes
    void StartAllPhysics();
    void StopAllPhysics();
    void StartAllScripts();
    void StopAllScripts();
    void StartAllAnimations();
    void StopAllAnimations();
    void StartAllSpriteAnimations();
    void StopAllSpriteAnimations();
    void StartAllAudio();
    void StopAllAudio();
    void StartAllParticles();
    void StopAllParticles();

    // Snapshot / restore all scenes for play mode
    struct SceneSnapshot {
        std::string YamlData;
        std::string Name;
        std::string FilePath;
        bool PhysicsEnabled;
    };
    std::vector<SceneSnapshot> SnapshotAll() const;
    void RestoreAll(const std::vector<SceneSnapshot>& snapshots);

private:
    std::vector<LoadedSceneInfo> m_LoadedScenes;
    int m_ActiveSceneIndex = -1;

    // Transition state
    TransitionPhase m_TransitionPhase = TransitionPhase::None;
    TransitionType  m_TransitionType  = TransitionType::None;
    float m_TransitionDuration = 1.0f;
    float m_TransitionProgress = 0.0f; // 0..1 within current phase
    float m_TransitionAlpha    = 0.0f; // overlay alpha for rendering
    std::string m_TransitionTargetPath;

    // Callback when transition completes loading
    void FinishTransitionLoad();
};

} // namespace VE
