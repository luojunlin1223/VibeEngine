/*
 * VibeEngine Standalone Runtime — Runs a packaged game without the editor.
 *
 * Loads a startup scene, finds the main camera, and renders the game
 * in a single window. Supports scripts, physics, audio, and all
 * engine features available in Play mode.
 */
#include <VibeEngine/VibeEngine.h>
#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <yaml-cpp/yaml.h>
#include <filesystem>
#include <fstream>
#include <limits>

class GameRuntime : public VE::Application {
public:
    GameRuntime()
        : VE::Application(VE::RendererAPI::API::OpenGL)
    {
        VE_INFO("VibeEngine Runtime starting...");
        VE::AudioEngine::Init();
        VE::ScriptEngine::Init();

        VE::MeshLibrary::Init();
        VE::SpriteBatchRenderer::Init();
        VE::ParticleRenderer::Init();
        VE::InstancedRenderer::Init();

        m_Scene = std::make_shared<VE::Scene>();

        // Load startup scene
        std::string scenePath = FindStartupScene();
        if (!scenePath.empty()) {
            VE::SceneSerializer serializer(m_Scene);
            if (serializer.Deserialize(scenePath)) {
                VE_INFO("Loaded scene: {}", scenePath);
            } else {
                VE_ERROR("Failed to load scene: {}", scenePath);
            }
        } else {
            VE_ERROR("No startup scene found!");
        }

        // Load script DLL if present
        std::string dllPath = FindScriptDLL();
        if (!dllPath.empty()) {
            if (VE::ScriptEngine::LoadScriptDLL(dllPath))
                VE_INFO("Loaded scripts: {}", dllPath);
        }

        // Start game systems
        StartGame();

        VE_INFO("VibeEngine Runtime ready");
    }

    ~GameRuntime() override {
        StopGame();
        VE::InstancedRenderer::Shutdown();
        VE::ParticleRenderer::Shutdown();
        VE::SpriteBatchRenderer::Shutdown();
        VE::ScriptEngine::Shutdown();
        VE::AudioEngine::Shutdown();
        VE_INFO("VibeEngine Runtime shutdown");
    }

protected:
    void OnUpdate() override {
        m_Scene->OnUpdate(m_DeltaTime);

        // Update audio listener from main camera
        if (m_MainCamera != entt::null) {
            glm::mat4 worldXform = m_Scene->GetWorldTransform(m_MainCamera);
            glm::vec3 camPos = glm::vec3(worldXform[3]);
            glm::mat4 viewMat = VE::Scene::ComputeCameraView(worldXform);
            glm::vec3 forward = -glm::vec3(viewMat[0][2], viewMat[1][2], viewMat[2][2]);
            glm::vec3 up = glm::vec3(viewMat[0][1], viewMat[1][1], viewMat[2][1]);
            float pos[3] = { camPos.x, camPos.y, camPos.z };
            float fwd[3] = { forward.x, forward.y, forward.z };
            float u[3] = { up.x, up.y, up.z };
            m_Scene->UpdateAudio(pos, fwd, u);
        }
    }

    void OnRender() override {
        FindMainCamera();
        if (m_MainCamera == entt::null) return;

        auto& cam = m_Scene->GetRegistry().get<VE::CameraComponent>(m_MainCamera);
        glm::mat4 worldXform = m_Scene->GetWorldTransform(m_MainCamera);

        int fbW, fbH;
        glfwGetFramebufferSize(GetWindow().GetNativeWindow(), &fbW, &fbH);
        if (fbW <= 0 || fbH <= 0) return;
        float aspect = static_cast<float>(fbW) / static_cast<float>(fbH);

        glm::mat4 gameView = VE::Scene::ComputeCameraView(worldXform);
        glm::mat4 gameProj = VE::Scene::ComputeCameraProjection(
            static_cast<int>(cam.ProjectionType), cam.FOV, cam.Size,
            cam.NearClip, cam.FarClip, aspect);
        glm::mat4 gameVP = gameProj * gameView;
        glm::vec3 gameCamPos = glm::vec3(worldXform[3]);

        VE::RenderCommand::SetViewport(0, 0, fbW, fbH);

        // Update script camera matrices for ScreenToWorldRay
        VE::SetScriptCameraMatrices(gameView, gameProj,
                                     static_cast<float>(fbW), static_cast<float>(fbH));

        // Shadow pass
        if (m_Scene->GetPipelineSettings().ShadowEnabled) {
            m_Scene->ComputeShadows(gameView, gameProj, cam.NearClip, cam.FarClip);
        }

        // Sky
        glm::mat4 skyView = glm::mat4(glm::mat3(gameView));
        m_Scene->OnRenderSky(gameProj * skyView);

        // Scene
        m_Scene->OnRender(gameVP, gameCamPos);
        m_Scene->OnRenderSprites(gameVP);
        m_Scene->OnRenderParticles(gameVP, gameCamPos);
    }

    void OnImGuiRender() override {
        // No editor UI in runtime — ImGui still present for potential debug overlay
    }

private:
    std::shared_ptr<VE::Scene> m_Scene;
    entt::entity m_MainCamera = entt::null;

    std::string FindStartupScene() {
        // 1. Check for startup.vscene (convention)
        if (std::filesystem::exists("Assets/startup.vscene"))
            return "Assets/startup.vscene";

        // 2. Check ProjectSettings for last scene
        if (std::filesystem::exists("ProjectSettings/GameSettings.yaml")) {
            try {
                YAML::Node root = YAML::LoadFile("ProjectSettings/GameSettings.yaml");
                if (root["StartupScene"])
                    return root["StartupScene"].as<std::string>();
            } catch (...) {}
        }

        // 3. Find first .vscene file in Assets/
        if (std::filesystem::exists("Assets")) {
            for (auto& entry : std::filesystem::directory_iterator("Assets")) {
                if (entry.path().extension() == ".vscene")
                    return entry.path().string();
            }
        }

        return "";
    }

    std::string FindScriptDLL() {
        // Look for GameScripts.dll in common locations
        const char* paths[] = {
            "GameScripts.dll",
            "Scripts/GameScripts.dll",
            "ScriptBuild/out/Release/GameScripts.dll",
            "ScriptBuild/out/Debug/GameScripts.dll"
        };
        for (auto& p : paths) {
            if (std::filesystem::exists(p)) return p;
        }
        return "";
    }

    void FindMainCamera() {
        if (m_MainCamera != entt::null && m_Scene->GetRegistry().valid(m_MainCamera))
            return;

        m_MainCamera = entt::null;
        auto camView = m_Scene->GetAllEntitiesWith<VE::TagComponent, VE::TransformComponent, VE::CameraComponent>();
        int highestPriority = std::numeric_limits<int>::min();
        int camCount = 0;
        entt::entity singleCam = entt::null;

        for (auto e : camView) {
            camCount++;
            singleCam = e;
            auto& tag = camView.get<VE::TagComponent>(e);
            if (tag.GameObjectTag == "MainCamera") {
                auto& cam = camView.get<VE::CameraComponent>(e);
                if (cam.Priority >= highestPriority) {
                    highestPriority = cam.Priority;
                    m_MainCamera = e;
                }
            }
        }
        // If only one camera, use it regardless of tag
        if (camCount == 1)
            m_MainCamera = singleCam;
    }

    void StartGame() {
        m_Scene->StartPhysics();
        m_Scene->StartScripts();
        m_Scene->StartAnimations();
        m_Scene->StartSpriteAnimations();
        m_Scene->StartAudio();
        m_Scene->StartParticles();
    }

    void StopGame() {
        m_Scene->StopPhysics();
        m_Scene->StopScripts();
        m_Scene->StopAnimations();
        m_Scene->StopSpriteAnimations();
        m_Scene->StopAudio();
        m_Scene->StopParticles();
    }
};

int main() {
    GameRuntime app;
    app.Run();
    return 0;
}
