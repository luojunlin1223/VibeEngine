/*
 * SceneManager — Implementation of multi-scene management with
 * additive loading and scene transitions.
 */
#include "VibeEngine/Scene/SceneManager.h"
#include "VibeEngine/Scene/SceneSerializer.h"
#include "VibeEngine/Core/Log.h"

#include <algorithm>
#include <filesystem>

namespace VE {

// ── Scene loading ───────────────────────────────────────────────────

int SceneManager::LoadScene(const std::string& path, SceneLoadMode mode) {
    auto scene = std::make_shared<Scene>();
    SceneSerializer serializer(scene);
    if (!serializer.Deserialize(path)) {
        VE_ERROR("SceneManager: Failed to load scene from '{0}'", path);
        return -1;
    }

    // Derive display name from filename
    std::string name = std::filesystem::path(path).stem().string();

    if (mode == SceneLoadMode::Single) {
        // Replace all scenes
        m_LoadedScenes.clear();
        m_ActiveSceneIndex = -1;
    }

    LoadedSceneInfo info;
    info.ScenePtr = scene;
    info.Name = name;
    info.FilePath = path;
    info.PhysicsEnabled = true;

    m_LoadedScenes.push_back(std::move(info));
    int index = static_cast<int>(m_LoadedScenes.size()) - 1;

    // In Single mode, or if this is the first scene, set as active
    if (mode == SceneLoadMode::Single || m_ActiveSceneIndex < 0) {
        m_ActiveSceneIndex = index;
    }

    VE_INFO("SceneManager: Loaded scene '{0}' (index {1}, mode={2})",
            name, index, mode == SceneLoadMode::Single ? "Single" : "Additive");
    return index;
}

int SceneManager::AddScene(std::shared_ptr<Scene> scene, const std::string& name,
                           const std::string& filePath) {
    LoadedSceneInfo info;
    info.ScenePtr = scene;
    info.Name = name;
    info.FilePath = filePath;
    info.PhysicsEnabled = true;

    m_LoadedScenes.push_back(std::move(info));
    int index = static_cast<int>(m_LoadedScenes.size()) - 1;

    if (m_ActiveSceneIndex < 0)
        m_ActiveSceneIndex = index;

    return index;
}

bool SceneManager::UnloadScene(int index) {
    if (index < 0 || index >= static_cast<int>(m_LoadedScenes.size())) {
        VE_WARN("SceneManager: Invalid scene index {0}", index);
        return false;
    }

    if (m_LoadedScenes.size() <= 1) {
        VE_WARN("SceneManager: Cannot unload the only loaded scene");
        return false;
    }

    std::string name = m_LoadedScenes[index].Name;
    m_LoadedScenes.erase(m_LoadedScenes.begin() + index);

    // Adjust active scene index
    if (m_ActiveSceneIndex == index) {
        m_ActiveSceneIndex = 0; // fallback to first
    } else if (m_ActiveSceneIndex > index) {
        m_ActiveSceneIndex--;
    }

    VE_INFO("SceneManager: Unloaded scene '{0}'", name);
    return true;
}

void SceneManager::UnloadAllScenes() {
    m_LoadedScenes.clear();
    m_ActiveSceneIndex = -1;
}

// ── Active scene ────────────────────────────────────────────────────

std::shared_ptr<Scene> SceneManager::GetActiveScene() const {
    if (m_ActiveSceneIndex >= 0 && m_ActiveSceneIndex < static_cast<int>(m_LoadedScenes.size()))
        return m_LoadedScenes[m_ActiveSceneIndex].ScenePtr;
    return nullptr;
}

void SceneManager::SetActiveScene(int index) {
    if (index >= 0 && index < static_cast<int>(m_LoadedScenes.size())) {
        m_ActiveSceneIndex = index;
        VE_INFO("SceneManager: Active scene set to '{0}' (index {1})",
                m_LoadedScenes[index].Name, index);
    }
}

std::string SceneManager::GetActiveSceneName() const {
    if (m_ActiveSceneIndex >= 0 && m_ActiveSceneIndex < static_cast<int>(m_LoadedScenes.size()))
        return m_LoadedScenes[m_ActiveSceneIndex].Name;
    return "";
}

// ── Update / Render ─────────────────────────────────────────────────

void SceneManager::OnUpdateAll(float deltaTime) {
    for (int i = 0; i < static_cast<int>(m_LoadedScenes.size()); i++) {
        auto& info = m_LoadedScenes[i];
        // Only update scenes that have physics enabled (or are the active scene)
        info.ScenePtr->OnUpdate(deltaTime);
    }
}

void SceneManager::OnRenderAllSky(const glm::mat4& skyViewProjection) {
    // Only render sky from the active scene (sky is a global effect)
    auto active = GetActiveScene();
    if (active)
        active->OnRenderSky(skyViewProjection);
}

void SceneManager::OnRenderAll(const glm::mat4& viewProjection, const glm::vec3& cameraPos,
                               uint32_t viewportWidth, uint32_t viewportHeight) {
    for (auto& info : m_LoadedScenes) {
        info.ScenePtr->OnRenderDeferred(viewProjection, cameraPos, viewportWidth, viewportHeight);
    }
}

void SceneManager::OnRenderAllTerrain(const glm::mat4& viewProjection, const glm::vec3& cameraPos) {
    for (auto& info : m_LoadedScenes) {
        info.ScenePtr->OnRenderTerrain(viewProjection, cameraPos);
    }
}

void SceneManager::OnRenderAllSprites(const glm::mat4& viewProjection) {
    for (auto& info : m_LoadedScenes) {
        info.ScenePtr->OnRenderSprites(viewProjection);
    }
}

void SceneManager::OnRenderAllParticles(const glm::mat4& viewProjection, const glm::vec3& cameraPos) {
    for (auto& info : m_LoadedScenes) {
        info.ScenePtr->OnRenderParticles(viewProjection, cameraPos);
    }
}

void SceneManager::OnRenderAllUI(uint32_t screenWidth, uint32_t screenHeight,
                                  float mouseX, float mouseY, bool mouseDown) {
    for (auto& info : m_LoadedScenes) {
        info.ScenePtr->OnRenderUI(screenWidth, screenHeight, mouseX, mouseY, mouseDown);
    }
}

// ── Transitions ─────────────────────────────────────────────────────

void SceneManager::TransitionToScene(const std::string& path,
                                      TransitionType type,
                                      float duration) {
    if (m_TransitionPhase != TransitionPhase::None) {
        VE_WARN("SceneManager: Transition already in progress, ignoring new request");
        return;
    }

    m_TransitionType = type;
    m_TransitionTargetPath = path;

    if (type == TransitionType::None) {
        // Instant switch
        LoadScene(path, SceneLoadMode::Single);
        return;
    }

    m_TransitionDuration = std::max(duration, 0.01f);
    m_TransitionProgress = 0.0f;
    m_TransitionAlpha = 0.0f;
    m_TransitionPhase = TransitionPhase::FadeOut;

    VE_INFO("SceneManager: Starting {0} transition to '{1}' ({2}s)",
            type == TransitionType::Fade ? "Fade" : "CrossFade",
            path, duration);
}

void SceneManager::UpdateTransition(float deltaTime) {
    if (m_TransitionPhase == TransitionPhase::None)
        return;

    float halfDuration = m_TransitionDuration * 0.5f;

    switch (m_TransitionPhase) {
    case TransitionPhase::FadeOut: {
        m_TransitionProgress += deltaTime / halfDuration;
        if (m_TransitionProgress >= 1.0f) {
            m_TransitionProgress = 1.0f;
            m_TransitionAlpha = 1.0f;
            m_TransitionPhase = TransitionPhase::Loading;
        } else {
            m_TransitionAlpha = m_TransitionProgress; // linear fade out
        }
        break;
    }
    case TransitionPhase::Loading: {
        // Load the new scene (single frame)
        FinishTransitionLoad();
        m_TransitionProgress = 0.0f;
        m_TransitionPhase = TransitionPhase::FadeIn;
        break;
    }
    case TransitionPhase::FadeIn: {
        m_TransitionProgress += deltaTime / halfDuration;
        if (m_TransitionProgress >= 1.0f) {
            m_TransitionProgress = 0.0f;
            m_TransitionAlpha = 0.0f;
            m_TransitionPhase = TransitionPhase::None;
            VE_INFO("SceneManager: Transition complete");
        } else {
            m_TransitionAlpha = 1.0f - m_TransitionProgress; // linear fade in
        }
        break;
    }
    default:
        break;
    }
}

void SceneManager::FinishTransitionLoad() {
    LoadScene(m_TransitionTargetPath, SceneLoadMode::Single);
    m_TransitionTargetPath.clear();
}

// ── Play mode helpers ───────────────────────────────────────────────

void SceneManager::StartAllPhysics() {
    for (auto& info : m_LoadedScenes) {
        if (info.PhysicsEnabled)
            info.ScenePtr->StartPhysics();
    }
}

void SceneManager::StopAllPhysics() {
    for (auto& info : m_LoadedScenes)
        info.ScenePtr->StopPhysics();
}

void SceneManager::StartAllScripts() {
    for (auto& info : m_LoadedScenes)
        info.ScenePtr->StartScripts();
}

void SceneManager::StopAllScripts() {
    for (auto& info : m_LoadedScenes)
        info.ScenePtr->StopScripts();
}

void SceneManager::StartAllAnimations() {
    for (auto& info : m_LoadedScenes)
        info.ScenePtr->StartAnimations();
}

void SceneManager::StopAllAnimations() {
    for (auto& info : m_LoadedScenes)
        info.ScenePtr->StopAnimations();
}

void SceneManager::StartAllSpriteAnimations() {
    for (auto& info : m_LoadedScenes)
        info.ScenePtr->StartSpriteAnimations();
}

void SceneManager::StopAllSpriteAnimations() {
    for (auto& info : m_LoadedScenes)
        info.ScenePtr->StopSpriteAnimations();
}

void SceneManager::StartAllAudio() {
    for (auto& info : m_LoadedScenes)
        info.ScenePtr->StartAudio();
}

void SceneManager::StopAllAudio() {
    for (auto& info : m_LoadedScenes)
        info.ScenePtr->StopAudio();
}

void SceneManager::StartAllParticles() {
    for (auto& info : m_LoadedScenes)
        info.ScenePtr->StartParticles();
}

void SceneManager::StopAllParticles() {
    for (auto& info : m_LoadedScenes)
        info.ScenePtr->StopParticles();
}

std::vector<SceneManager::SceneSnapshot> SceneManager::SnapshotAll() const {
    std::vector<SceneSnapshot> snapshots;
    for (auto& info : m_LoadedScenes) {
        SceneSnapshot snap;
        SceneSerializer serializer(info.ScenePtr);
        snap.YamlData = serializer.SerializeToString();
        snap.Name = info.Name;
        snap.FilePath = info.FilePath;
        snap.PhysicsEnabled = info.PhysicsEnabled;
        snapshots.push_back(std::move(snap));
    }
    return snapshots;
}

void SceneManager::RestoreAll(const std::vector<SceneSnapshot>& snapshots) {
    int prevActiveIndex = m_ActiveSceneIndex;
    m_LoadedScenes.clear();
    m_ActiveSceneIndex = -1;

    for (auto& snap : snapshots) {
        auto scene = std::make_shared<Scene>();
        SceneSerializer serializer(scene);
        serializer.DeserializeFromString(snap.YamlData);

        LoadedSceneInfo info;
        info.ScenePtr = scene;
        info.Name = snap.Name;
        info.FilePath = snap.FilePath;
        info.PhysicsEnabled = snap.PhysicsEnabled;
        m_LoadedScenes.push_back(std::move(info));
    }

    // Restore active index (clamped)
    if (!m_LoadedScenes.empty()) {
        m_ActiveSceneIndex = std::clamp(prevActiveIndex, 0,
                                         static_cast<int>(m_LoadedScenes.size()) - 1);
    }
}

} // namespace VE
