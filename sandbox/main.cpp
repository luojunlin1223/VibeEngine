#include <VibeEngine/VibeEngine.h>
#include <VibeEngine/Renderer/ShaderSources.h>
#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <yaml-cpp/yaml.h>
#include <limits>
#include <filesystem>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>
#endif
#include <fstream>
#include <iomanip>
#include <algorithm>
#include <array>
#include <any>
#include <set>
#include <cmath>
#include <cctype>
#include <unordered_map>

static const char* s_SceneFilter = "VibeEngine Scene (*.vscene)\0*.vscene\0All Files\0*.*\0";

enum class SceneGizmoTool {
    Translate,
    Rotate
};

static std::array<float, 3> EulerFromForwardDirection(const glm::vec3& rawDirection) {
    glm::vec3 direction = rawDirection;
    float len = glm::length(direction);
    if (len <= 0.0001f)
        direction = glm::normalize(glm::vec3(0.3f, 1.0f, 0.5f));
    else
        direction /= len;

    float yaw = std::asin(std::clamp(direction.x, -1.0f, 1.0f));
    float pitch = std::atan2(-direction.y, direction.z);
    return { glm::degrees(pitch), glm::degrees(yaw), 0.0f };
}

class Sandbox : public VE::Application {
public:
    explicit Sandbox(bool renderDiagnosticsOnce = false,
                      bool hpwaterMotionDiagnostics = false,
                      bool hpwaterFluidFilterDiagnostics = false,
                      bool hpwaterSSRDiagnostics = false)
        : VE::Application(VE::RendererAPI::API::OpenGL)
        , m_RenderDiagnosticsOnce(renderDiagnosticsOnce || hpwaterMotionDiagnostics || hpwaterFluidFilterDiagnostics || hpwaterSSRDiagnostics)
        , m_HPWaterMotionDiagnostics(hpwaterMotionDiagnostics)
        , m_HPWaterFluidFilterDiagnostics(hpwaterFluidFilterDiagnostics)
        , m_HPWaterSSRDiagnostics(hpwaterSSRDiagnostics)
    {
        VE_INFO("Sandbox application created");
        VE::AudioEngine::Init();
        VE::ScriptEngine::Init();
        VE::ScriptEngine::SetEngineIncludePath(std::string(VE_PROJECT_ROOT) + "/engine/include");

        // Auto-build script DLL on startup if scripts exist
        if (VE::ScriptEngine::HasScriptFiles()) {
            VE_INFO("Auto-building script project...");
            VE::ScriptEngine::BuildScriptProjectSync();
        }

        VE::MeshLibrary::Init();
        VE::SpriteBatchRenderer::Init();
        VE::ParticleRenderer::Init();
        VE::InstancedRenderer::Init();
        VE::OcclusionCulling::Init();
        VE::UIRenderer::Init();
        m_Scene = std::make_shared<VE::Scene>();
        m_SceneManager.AddScene(m_Scene, "Untitled");
        VE::ScriptEngine::SetSceneManager(&m_SceneManager);
        VE::ScriptEngine::SetupScriptFileWatcher();

        // Editor plugins
        VE::PluginEngine::Init();
        // Plugin source path relative to CWD (build/bin/Debug/), same as Scripts
        VE::PluginEngine::SetPluginSourcePath("Assets/Editor");
        VE::PluginEngine::SetEngineIncludePath(std::string(VE_PROJECT_ROOT) + "/engine/include");
        VE::PluginEngine::SetImGuiSourcePath(VE_IMGUI_SOURCE_DIR);
        if (VE::PluginEngine::HasPluginFiles()) {
            VE_INFO("Auto-building editor plugins...");
            VE::PluginEngine::BuildPluginProjectSync();
        }
        VE::PluginEngine::SetupFileWatcher();
        m_Camera.SetViewportSize(1280.0f, 720.0f);
        m_AssetDatabase.Init(".");
        ScanAndRegisterAssets();

        VE::FramebufferSpec fbSpec;
        fbSpec.Width = 1280;
        fbSpec.Height = 720;
        fbSpec.HDR = true;
        m_Framebuffer = VE::Framebuffer::Create(fbSpec);
        m_GameFramebuffer = VE::Framebuffer::Create(fbSpec);

        // Command history (undo/redo)
        m_CommandHistory.SetScene(&m_Scene);
        m_CommandHistory.SetRestoreCallback([this]() {
            ClearSelection();
            uint64_t uuid = m_CommandHistory.GetSelectedEntityUUID();
            if (uuid != 0) {
                auto view = m_Scene->GetAllEntitiesWith<VE::IDComponent>();
                for (auto e : view) {
                    auto& id = view.get<VE::IDComponent>(e);
                    if (static_cast<uint64_t>(id.ID) == uuid) {
                        SelectEntityOnly(VE::Entity(e, &*m_Scene));
                        break;
                    }
                }
            }
        });
        m_CommandHistory.SetDirtyCallback([this]() { MarkDirty(); });

        // Set up default input action map
        SetupDefaultInputActions();

        // Restore last session (scene + camera)
        LoadEditorSettings();

        if (m_HPWaterMotionDiagnostics) {
            m_RenderDiagnosticsOnceMinFrame = 60;
            m_RenderDiagnosticsOnceMaxFrame = 240;
            m_RenderDiagnosticsRequireObjectMotion = true;
            LoadSceneFromPath((std::filesystem::path(VE_PROJECT_ROOT) / "Assets" / "launcher.vscene").generic_string());
            if (!m_PlayMode)
                EnterPlayMode();
        }

        if (m_HPWaterFluidFilterDiagnostics) {
            m_RenderDiagnosticsOnceMinFrame = 60;
            m_RenderDiagnosticsOnceMaxFrame = 240;
            m_RenderDiagnosticsRequireFluidFiltering = true;
            LoadSceneFromPath((std::filesystem::path(VE_PROJECT_ROOT) / "Assets" / "launcher.vscene").generic_string());
            if (!m_PlayMode)
                EnterPlayMode();
        }

        if (m_HPWaterSSRDiagnostics) {
            m_RenderDiagnosticsOnceMinFrame = 60;
            m_RenderDiagnosticsOnceMaxFrame = 240;
            m_RenderDiagnosticsRequireSSR = true;
            LoadSceneFromPath((std::filesystem::path(VE_PROJECT_ROOT) / "Assets" / "launcher.vscene").generic_string());
            auto& ps = m_Scene->GetPipelineSettings();
            ps.SSREnabled = true;
            ps.SSRMaxSteps = std::max(ps.SSRMaxSteps, 128);
            ps.SSRStepSize = std::clamp(ps.SSRStepSize, 0.01f, 0.05f);
            ps.SSRThickness = std::clamp(ps.SSRThickness, 0.25f, 0.75f);
            ps.SSRMaxDistance = std::max(ps.SSRMaxDistance, 80.0f);
            if (!m_PlayMode)
                EnterPlayMode();
        }
    }

    ~Sandbox() override {
        SaveEditorSettings();

        // Release GPU resources before renderer shutdown
        m_Scene.reset();
        m_SceneManager.UnloadAllScenes();
        m_Framebuffer.reset();
        m_GameFramebuffer.reset();
        m_PostProcessing.Shutdown();
        m_GamePostProcessing.Shutdown();
        m_ThumbnailCache.Clear();

        VE::MeshLibrary::Shutdown();
        VE::MeshImporter::ClearCache();
        VE::UIRenderer::Shutdown();
        VE::OcclusionCulling::Shutdown();
        VE::InstancedRenderer::Shutdown();
        VE::ParticleRenderer::Shutdown();
        VE::SpriteBatchRenderer::Shutdown();
        VE::PluginEngine::Shutdown();
        VE::ScriptEngine::Shutdown();
        VE::AudioEngine::Shutdown();
        VE_INFO("Sandbox application destroyed");
    }

protected:
    void OnUpdate() override {
        // Pause support: skip scene update when paused (unless stepping)
        bool shouldUpdate = true;
        if (m_PlayMode && m_Paused && !m_StepOneFrame)
            shouldUpdate = false;

        if (shouldUpdate) {
            m_Scene->OnUpdate(m_DeltaTime);
            UpdateLauncherPrototype(m_DeltaTime);
        }

        if (m_StepOneFrame) {
            m_StepOneFrame = false;
            m_Paused = true;
        }

        m_AssetDatabase.Update(m_DeltaTime);

        // Script file watcher + auto-build-on-save (runs in both edit and play mode)
        VE::ScriptEngine::UpdateScriptFileWatcher(m_DeltaTime);
        VE::ScriptEngine::CheckForReload();

        // Editor plugin file watcher + auto-build
        VE::PluginEngine::UpdateFileWatcher(m_DeltaTime);
        VE::PluginEngine::CheckForReload();

        // Auto-save timer (only outside play mode and when scene is dirty)
        if (m_AutoSaveEnabled && !m_PlayMode && m_SceneDirty) {
            m_AutoSaveTimer += m_DeltaTime;
            if (m_AutoSaveTimer >= m_AutoSaveInterval) {
                m_AutoSaveTimer = 0.0f;
                std::filesystem::create_directories("ProjectSettings");
                VE::SceneSerializer serializer(m_Scene);
                serializer.Serialize("ProjectSettings/AutoSave.vscene");
                VE_INFO("Auto-saved scene to ProjectSettings/AutoSave.vscene");
            }
        }

        if (m_PlayMode) {

            // Update 3D audio listener from camera
            glm::vec3 camPos = (m_Camera.GetMode() == VE::CameraMode::Perspective3D)
                ? m_Camera.GetPosition3D()
                : glm::vec3(m_Camera.GetPosition(), 5.0f);
            // Use a simple forward vector; for the editor camera the view matrix encodes this
            glm::vec3 forward(0.0f, 0.0f, -1.0f);
            glm::vec3 up(0.0f, 1.0f, 0.0f);
            if (m_Camera.GetMode() == VE::CameraMode::Perspective3D) {
                // Extract forward/up from view matrix
                auto view = m_Camera.GetViewMatrix();
                forward = -glm::vec3(view[0][2], view[1][2], view[2][2]);
                up      =  glm::vec3(view[0][1], view[1][1], view[2][1]);
            }
            float pos[3] = { camPos.x, camPos.y, camPos.z };
            float fwd[3] = { forward.x, forward.y, forward.z };
            float u[3]   = { up.x, up.y, up.z };
            {
                PROFILE_SCOPE("Audio");
                m_Scene->UpdateAudio(pos, fwd, u);
            }
        }
    }

    void RenderSelectedOutline() {
        if (!m_OutlineEnabled || !m_SelectedEntity) return;
        if (!m_SelectedEntity.HasComponent<VE::TransformComponent>()) return;

        std::shared_ptr<VE::VertexArray> vao;
        if (m_SelectedEntity.HasComponent<VE::MeshRendererComponent>())
            vao = m_SelectedEntity.GetComponent<VE::MeshRendererComponent>().Mesh;
        if (m_SelectedEntity.HasComponent<VE::AnimatorComponent>()) {
            auto& ac = m_SelectedEntity.GetComponent<VE::AnimatorComponent>();
            if (ac._Animator && ac._Animator->GetSkinnedVAO())
                vao = ac._Animator->GetSkinnedVAO();
        }
        if (!vao) return;

        // Inline outline shader: extrudes back-face vertices along normals (Cull Front)
        static GLuint s_OutlineProg = 0;
        if (s_OutlineProg == 0) {
            const char* vs = R"(
                #version 460 core
                layout(location = 0) in vec3 a_Position;
                layout(location = 1) in vec3 a_Normal;
                uniform mat4 u_MVP;
                uniform mat4 u_Model;
                uniform vec2 u_ViewportSize;
                uniform float u_OutlinePixels;
                void main() {
                    gl_Position = u_MVP * vec4(a_Position, 1.0);
                    mat3 normalMat = mat3(u_Model);
                    vec3 clipNorm = mat3(u_MVP) * a_Normal;
                    vec2 offset = normalize(clipNorm.xy) * (u_OutlinePixels / u_ViewportSize) * 2.0;
                    gl_Position.xy += offset * gl_Position.w;
                }
            )";
            const char* fs = R"(
                #version 460 core
                uniform vec4 u_OutlineColor;
                out vec4 FragColor;
                void main() { FragColor = u_OutlineColor; }
            )";
            auto compileShader = [](GLenum type, const char* src) -> GLuint {
                GLuint s = glCreateShader(type);
                glShaderSource(s, 1, &src, nullptr);
                glCompileShader(s);
                return s;
            };
            GLuint vsID = compileShader(GL_VERTEX_SHADER, vs);
            GLuint fsID = compileShader(GL_FRAGMENT_SHADER, fs);
            s_OutlineProg = glCreateProgram();
            glAttachShader(s_OutlineProg, vsID);
            glAttachShader(s_OutlineProg, fsID);
            glLinkProgram(s_OutlineProg);
            glDeleteShader(vsID);
            glDeleteShader(fsID);
        }

        glm::mat4 model = m_Scene->GetWorldTransform(m_SelectedEntity.GetHandle());
        glm::mat4 mvp = m_FrameVP * model;
        uint32_t fbW = m_Framebuffer ? m_Framebuffer->GetWidth() : 1280;
        uint32_t fbH = m_Framebuffer ? m_Framebuffer->GetHeight() : 720;

        // Draw back-faces only, extruded along normals
        glUseProgram(s_OutlineProg);
        glUniformMatrix4fv(glGetUniformLocation(s_OutlineProg, "u_MVP"), 1, GL_FALSE, glm::value_ptr(mvp));
        glUniformMatrix4fv(glGetUniformLocation(s_OutlineProg, "u_Model"), 1, GL_FALSE, glm::value_ptr(model));
        glUniform2f(glGetUniformLocation(s_OutlineProg, "u_ViewportSize"), (float)fbW, (float)fbH);
        glUniform1f(glGetUniformLocation(s_OutlineProg, "u_OutlinePixels"), 3.0f);
        glUniform4f(glGetUniformLocation(s_OutlineProg, "u_OutlineColor"), 1.0f, 0.5f, 0.0f, 1.0f);

        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT);       // draw back-faces only
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_FALSE);      // don't write depth

        vao->Bind();
        glDrawElements(GL_TRIANGLES,
            static_cast<GLsizei>(vao->GetIndexBuffer()->GetCount()),
            GL_UNSIGNED_INT, nullptr);

        // Restore GL state
        glCullFace(GL_BACK);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glUseProgram(0);
    }

    VE::PostProcessSettings BuildPostProcessSettings() const {
        auto& ps = m_Scene->GetPipelineSettings();
        VE::PostProcessSettings s;
        s.Bloom    = { ps.BloomEnabled, ps.BloomThreshold, ps.BloomIntensity, ps.BloomIterations };
        s.Vignette = { ps.VignetteEnabled, ps.VignetteIntensity, ps.VignetteSmoothness };
        s.Color    = { ps.ColorAdjustEnabled, ps.ColorExposure, ps.ColorContrast,
                       ps.ColorSaturation, ps.ColorFilter, ps.ColorGamma };
        s.SMH      = { ps.SMHEnabled, ps.SMH_Shadows, ps.SMH_Midtones, ps.SMH_Highlights,
                       ps.SMH_ShadowStart, ps.SMH_ShadowEnd, ps.SMH_HighlightStart, ps.SMH_HighlightEnd };
        s.Curves.Enabled       = ps.CurvesEnabled;
        s.Curves.Master.Points = ps.CurvesMaster;
        s.Curves.Red.Points    = ps.CurvesRed;
        s.Curves.Green.Points  = ps.CurvesGreen;
        s.Curves.Blue.Points   = ps.CurvesBlue;
        // HDR pipeline: when HDR is enabled, force tonemapping on with HDR settings
        if (ps.HDREnabled) {
            s.Tonemap = { true, static_cast<VE::TonemapMode>(ps.ToneMapMode + 1) }; // +1 because TonemapMode enum: 0=None,1=Reinhard,2=ACES,3=Uncharted2
            // Apply HDR exposure via color adjustments (additive with existing exposure)
            s.Color.Enabled = true;
            s.Color.Exposure += std::log2(ps.Exposure); // convert linear multiplier to EV
        } else {
            s.Tonemap = { ps.TonemapEnabled, static_cast<VE::TonemapMode>(ps.TonemapMode) };
        }
        // Fog
        s.Fog = { ps.FogEnabled, static_cast<VE::FogMode>(ps.FogMode), ps.FogColor,
                  ps.FogDensity, ps.FogStart, ps.FogEnd, ps.FogHeightFalloff, ps.FogMaxOpacity };
        // Volumetric fog
        s.VolumetricFog = { ps.VolFogEnabled, ps.VolFogDensity, ps.VolFogScattering,
                            ps.VolFogLightIntensity, ps.VolFogColor, ps.VolFogSteps,
                            ps.VolFogMaxDistance, ps.VolFogHeightFalloff, ps.VolFogBaseHeight };
        // Depth of Field
        s.DoF = { ps.DoFEnabled, ps.DoFFocusDistance, ps.DoFFocusRange,
                  ps.DoFMaxBlur, ps.DoFApertureSize };
        // Motion Blur
        if (ps.MotionBlurEnabled) {
            s.MotionBlur.Enabled    = true;
            s.MotionBlur.Strength   = ps.MotionBlurStrength;
            s.MotionBlur.NumSamples = ps.MotionBlurSamples;
            s.MotionBlur.InvViewProj  = glm::inverse(m_FrameVP);
            s.MotionBlur.PrevViewProj = m_PrevFrameVP;
        }
        s.NearClip = m_Camera.GetNearClip();
        s.FarClip  = m_Camera.GetFarClip();
        s.InvProjection = glm::inverse(m_Camera.GetProjectionMatrix());
        s.InvView       = glm::inverse(m_Camera.GetViewMatrix());
        // Find directional light for volumetric fog god rays
        {
            auto lv = m_Scene->GetAllEntitiesWith<VE::DirectionalLightComponent>();
            for (auto e : lv) {
                s.LightDir = m_Scene->GetEntityForward(e);
                break;
            }
        }
        // Anti-aliasing
        if (ps.AAMode == 4) // FXAA
            s.FXAA = { true, ps.FXAAEdgeThreshold, ps.FXAAEdgeThresholdMin, ps.FXAASubpixelQuality };
        if (ps.AAMode == 5) // TAA
            s.TAA = { true, ps.TAABlendFactor };
        return s;
    }

    void OnRender() override {
        // Check if MSAA sample count changed and recreate framebuffers
        auto& pipeSettings = m_Scene->GetPipelineSettings();
        uint32_t wantedSamples = 1;
        if (pipeSettings.AAMode == 1) wantedSamples = 2;
        else if (pipeSettings.AAMode == 2) wantedSamples = 4;
        else if (pipeSettings.AAMode == 3) wantedSamples = 8;
        if (wantedSamples != m_CurrentMSAASamples && m_Framebuffer) {
            m_CurrentMSAASamples = wantedSamples;
            VE::FramebufferSpec fbSpec;
            fbSpec.Width = m_Framebuffer->GetWidth();
            fbSpec.Height = m_Framebuffer->GetHeight();
            fbSpec.HDR = true;
            fbSpec.Samples = wantedSamples;
            m_Framebuffer = VE::Framebuffer::Create(fbSpec);
            m_GameFramebuffer = VE::Framebuffer::Create(fbSpec);
            m_PostProcessing.Shutdown();
            m_GamePostProcessing.Shutdown();
        }

        m_PrevFrameVP = m_FrameVP;
        m_FrameVP = m_Camera.GetViewProjection();
        glm::vec3 camPos = (m_Camera.GetMode() == VE::CameraMode::Perspective3D)
            ? m_Camera.GetPosition3D()
            : glm::vec3(m_Camera.GetPosition(), 5.0f);

        uint32_t fbW = m_Framebuffer ? m_Framebuffer->GetWidth()  : 1280;
        uint32_t fbH = m_Framebuffer ? m_Framebuffer->GetHeight() : 720;

        // Update script camera matrices for ScreenToWorldRay
        VE::SetScriptCameraMatrices(m_Camera.GetViewMatrix(), m_Camera.GetProjectionMatrix(),
                                     static_cast<float>(fbW), static_cast<float>(fbH));
        bool perspective3D = (m_Camera.GetMode() == VE::CameraMode::Perspective3D);

        VE::RenderGraph rg;
        rg.SetViewportSize(fbW, fbH);
        VE::RGHandle sceneColor;

        // Pass 1: Sky
        rg.AddPass("Sky", [&](VE::RGBuilder& b) {
            sceneColor = b.Import("SceneHDR",
                m_Framebuffer ? static_cast<uint32_t>(m_Framebuffer->GetColorAttachmentID()) : 0,
                fbW, fbH);
            b.Write(sceneColor);
            b.SideEffect();
        }, [&](const VE::RGResources&) {
            if (m_Framebuffer) {
                m_Framebuffer->Bind();
                VE::RenderCommand::SetDepthWrite(true);
                VE::RenderCommand::SetClearColor(0.1f, 0.1f, 0.1f, 1.0f);
                VE::RenderCommand::Clear();
            }
            // Sky rendered after deferred blit (in Opaque pass) using depth test
        });

        // Pass 2: Opaque geometry (deferred pipeline)
        rg.AddPass("Opaque", [&](VE::RGBuilder& b) {
            b.Write(sceneColor);
            b.SideEffect();
        }, [&](const VE::RGResources&) {
            // Deferred rendering: G-buffer pass + lighting pass
            m_Scene->OnRenderDeferred(m_FrameVP,
                m_Camera.GetViewMatrix(), m_Camera.GetProjectionMatrix(),
                camPos, m_Camera.GetNearClip(), m_Camera.GetFarClip(),
                fbW, fbH);

            // If debug view is active, replace lighting output with debug visualization
            auto& dr = m_Scene->GetDeferredRenderer();
            if (m_Scene->GetPipelineSettings().GBufferDebugView > 0 && dr.IsInitialized()) {
                dr.DebugVisualize(static_cast<VE::GBufferDebugView>(m_Scene->GetPipelineSettings().GBufferDebugView));
            }

            // Blit deferred lighting output into the scene framebuffer
            uint32_t deferredOutput = dr.GetOutputTexture();
            if (deferredOutput && m_Framebuffer) {
                m_Framebuffer->Bind();

                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, deferredOutput);
                glDisable(GL_DEPTH_TEST);
                glDepthMask(GL_FALSE);

                static std::shared_ptr<VE::Shader> s_BlitShader;
                if (!s_BlitShader) {
                    static const char* blitFrag = R"(
#version 450 core
layout(location = 0) in vec2 v_UV;
layout(location = 0) out vec4 FragColor;
uniform sampler2D u_Source;
void main() { FragColor = texture(u_Source, v_UV); }
)";
                    s_BlitShader = VE::Shader::Create(VE::QuadVertexShaderSrc, blitFrag);
                }
                if (s_BlitShader) {
                    s_BlitShader->Bind();
                    s_BlitShader->SetInt("u_Source", 0);
                    static GLuint blitVAO = 0;
                    if (!blitVAO) glGenVertexArrays(1, &blitVAO);
                    glBindVertexArray(blitVAO);
                    glDrawArrays(GL_TRIANGLES, 0, 3);
                    glBindVertexArray(0);
                }

                glEnable(GL_DEPTH_TEST);
                glDepthMask(GL_TRUE);

                // Blit G-buffer depth to scene framebuffer so sky/terrain use correct depth
                {
                    GLint currentFBO;
                    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &currentFBO);
                    dr.BlitDepthTo(static_cast<uint32_t>(currentFBO), fbW, fbH);
                    m_Framebuffer->Bind();
                }
            }

            // Render sky AFTER deferred blit — depth test ensures sky only in empty areas
            m_Scene->OnRenderSky(m_Camera.GetSkyViewProjection());

            m_Scene->OnRenderTerrain(m_FrameVP, camPos);
        });

        // Pass 3: Decals (after opaque, before sprites)
        rg.AddPass("Decals", [&](VE::RGBuilder& b) {
            b.Write(sceneColor);
            b.SideEffect();
        }, [&](const VE::RGResources&) {
            if (m_Framebuffer) {
                uint32_t depthTex = static_cast<uint32_t>(m_Framebuffer->GetDepthAttachmentID());
                m_Scene->OnRenderDecals(m_FrameVP, m_Camera.GetViewMatrix(),
                    m_Camera.GetProjectionMatrix(), depthTex,
                    m_Framebuffer->GetWidth(), m_Framebuffer->GetHeight());
            }
        });

        // Pass 4: Sprites
        rg.AddPass("Sprites", [&](VE::RGBuilder& b) {
            b.Write(sceneColor);
            b.SideEffect();
        }, [&](const VE::RGResources&) {
            m_Scene->OnRenderSprites(m_FrameVP);
        });

        // Pass 5: Particles
        rg.AddPass("Particles", [&](VE::RGBuilder& b) {
            b.Write(sceneColor);
            b.SideEffect();
        }, [&](const VE::RGResources&) {
            m_Scene->OnRenderParticles(m_FrameVP, camPos);
        });

        // Pass 6: Outline (conditional on selection)
        if (m_OutlineEnabled && m_SelectedEntity) {
            rg.AddPass("Outline", [&](VE::RGBuilder& b) {
                b.Write(sceneColor);
                b.SideEffect();
            }, [&](const VE::RGResources&) {
                RenderSelectedOutline();
            });
        }

        // Pass 7: PostProcess
        rg.AddPass("PostProcess", [&](VE::RGBuilder& b) {
            b.Read(sceneColor);
            b.SideEffect();
        }, [&](const VE::RGResources&) {
            if (m_Framebuffer) {
                // Flush any pending gizmo 3D lines into framebuffer with depth testing
                // (lines were accumulated from the previous frame's gizmo drawing)
                if (!VE::GizmoRenderer::IsLines3DEmpty()) {
                    // Framebuffer is still bound from scene rendering
                    glEnable(GL_DEPTH_TEST);
                    glDepthFunc(GL_LEQUAL);
                    glDepthMask(GL_FALSE);
                    VE::GizmoRenderer::FlushLines3D(m_FrameVP);
                    glDepthMask(GL_TRUE);
                    glDepthFunc(GL_LESS);
                }
                m_Framebuffer->Unbind();
                if (m_Framebuffer->IsMultisampled())
                    m_Framebuffer->Resolve();

                auto ppSettings = BuildPostProcessSettings();
                uint32_t depthTex = static_cast<uint32_t>(m_Framebuffer->GetDepthAttachmentID());

                // Pass depth for fog and motion blur
                ppSettings.DepthTexture = depthTex;
                if (ppSettings.MotionBlur.Enabled)
                    ppSettings.MotionBlur.DepthTexture = depthTex;

                // Compute SSAO if enabled
                auto& ps = m_Scene->GetPipelineSettings();
                if (ps.SSAOEnabled) {
                    VE::SSAOSettings ssaoSettings{ true, ps.SSAORadius, ps.SSAOBias,
                                                    ps.SSAOIntensity, ps.SSAOKernelSize };
                    ppSettings.SSAOTexture = m_SSAO.Compute(
                        depthTex, m_Framebuffer->GetWidth(), m_Framebuffer->GetHeight(),
                        m_Camera.GetProjectionMatrix(), m_Camera.GetViewMatrix(), ssaoSettings);
                }

                // Compute SSR if enabled
                uint32_t sceneTex = static_cast<uint32_t>(m_Framebuffer->GetColorAttachmentID());
                if (ps.SSREnabled) {
                    VE::SSRSettings ssrSettings{ true, ps.SSRMaxSteps, ps.SSRStepSize,
                                                  ps.SSRThickness, ps.SSRMaxDistance };
                    ppSettings.SSRTexture = m_SSR.Compute(
                        sceneTex, depthTex,
                        m_Framebuffer->GetWidth(), m_Framebuffer->GetHeight(),
                        m_Camera.GetProjectionMatrix(), m_Camera.GetViewMatrix(), ssrSettings);
                }
                m_PostProcessedTexture = m_PostProcessing.Apply(
                    sceneTex, m_Framebuffer->GetWidth(), m_Framebuffer->GetHeight(), ppSettings);
                if (m_PostProcessedTexture == sceneTex)
                    m_PostProcessedTexture = 0;
            }
        });

        // ── Game Camera Render Passes ──
        if (m_ShowGameView && m_GameFramebuffer) {
            entt::entity mainCamEntity = entt::null;
            auto camView = m_Scene->GetAllEntitiesWith<VE::TagComponent, VE::TransformComponent, VE::CameraComponent>();
            int camCount = 0;
            entt::entity singleCam = entt::null;
            int highestPriority = std::numeric_limits<int>::min();
            for (auto e : camView) {
                if (!m_Scene->IsEntityActiveInHierarchy(e)) continue;
                camCount++;
                singleCam = e;
                auto& tag = camView.get<VE::TagComponent>(e);
                if (tag.GameObjectTag == "MainCamera") {
                    auto& cam = camView.get<VE::CameraComponent>(e);
                    if (cam.Priority >= highestPriority) {
                        highestPriority = cam.Priority;
                        mainCamEntity = e;
                    }
                }
            }
            if (camCount == 1)
                mainCamEntity = singleCam;

            if (mainCamEntity != entt::null) {
                auto& cam = m_Scene->GetRegistry().get<VE::CameraComponent>(mainCamEntity);
                glm::mat4 worldXform = m_Scene->GetWorldTransform(mainCamEntity);
                float aspect = static_cast<float>(m_GameFramebuffer->GetWidth())
                             / static_cast<float>(m_GameFramebuffer->GetHeight());

                glm::mat4 gameView = VE::Scene::ComputeCameraView(worldXform);
                glm::mat4 gameProj = VE::Scene::ComputeCameraProjection(
                    static_cast<int>(cam.ProjectionType), cam.FOV, cam.Size,
                    cam.NearClip, cam.FarClip, aspect);
                glm::mat4 gameVP = gameProj * gameView;
                glm::vec3 gameCamPos = glm::vec3(worldXform[3]);

                VE::RGHandle gameSceneColor;

                // Game Pass 0: Sky
                rg.AddPass("GameSky", [&](VE::RGBuilder& b) {
                    gameSceneColor = b.Import("GameSceneHDR",
                        static_cast<uint32_t>(m_GameFramebuffer->GetColorAttachmentID()),
                        m_GameFramebuffer->GetWidth(), m_GameFramebuffer->GetHeight());
                    b.Write(gameSceneColor);
                    b.SideEffect();
                }, [&](const VE::RGResources&) {
                    m_GameFramebuffer->Bind();
                    VE::RenderCommand::SetDepthWrite(true);
                    VE::RenderCommand::SetClearColor(0.1f, 0.1f, 0.1f, 1.0f);
                    VE::RenderCommand::Clear();
                });

                // Game Pass 1: Opaque (deferred pipeline)
                rg.AddPass("GameOpaque", [&](VE::RGBuilder& b) {
                    b.Write(gameSceneColor);
                    b.SideEffect();
                }, [&](const VE::RGResources&) {
                    uint32_t gw = m_GameFramebuffer->GetWidth();
                    uint32_t gh = m_GameFramebuffer->GetHeight();
                    m_Scene->OnRenderDeferred(gameVP, gameView, gameProj,
                        gameCamPos, cam.NearClip, cam.FarClip, gw, gh);

                    auto& dr = m_Scene->GetDeferredRenderer();
                    uint32_t deferredOut = dr.GetOutputTexture();
                    if (deferredOut && m_GameFramebuffer) {
                        m_GameFramebuffer->Bind();
                        glActiveTexture(GL_TEXTURE0);
                        glBindTexture(GL_TEXTURE_2D, deferredOut);
                        glDisable(GL_DEPTH_TEST);
                        glDepthMask(GL_FALSE);

                        static std::shared_ptr<VE::Shader> s_GameBlitShader;
                        if (!s_GameBlitShader) {
                            static const char* blitFrag = R"(
#version 450 core
layout(location = 0) in vec2 v_UV;
layout(location = 0) out vec4 FragColor;
uniform sampler2D u_Source;
void main() { FragColor = texture(u_Source, v_UV); }
)";
                            s_GameBlitShader = VE::Shader::Create(VE::QuadVertexShaderSrc, blitFrag);
                        }
                        if (s_GameBlitShader) {
                            s_GameBlitShader->Bind();
                            s_GameBlitShader->SetInt("u_Source", 0);
                            static GLuint gameBlitVAO = 0;
                            if (!gameBlitVAO) glGenVertexArrays(1, &gameBlitVAO);
                            glBindVertexArray(gameBlitVAO);
                            glDrawArrays(GL_TRIANGLES, 0, 3);
                            glBindVertexArray(0);
                        }
                        glEnable(GL_DEPTH_TEST);
                        glDepthMask(GL_TRUE);

                        // Match the Scene View path: copy deferred depth back,
                        // then draw sky into pixels that were not covered by geometry.
                        {
                            GLint currentFBO;
                            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &currentFBO);
                            dr.BlitDepthTo(static_cast<uint32_t>(currentFBO), gw, gh);
                            m_GameFramebuffer->Bind();
                        }
                    }
                    glm::mat4 skyView = glm::mat4(glm::mat3(gameView));
                    m_Scene->OnRenderSky(gameProj * skyView);
                    m_Scene->OnRenderTerrain(gameVP, gameCamPos);
                });

                // Game Pass 2: Decals
                rg.AddPass("GameDecals", [&](VE::RGBuilder& b) {
                    b.Write(gameSceneColor);
                    b.SideEffect();
                }, [&](const VE::RGResources&) {
                    if (m_GameFramebuffer) {
                        uint32_t gDepth = static_cast<uint32_t>(m_GameFramebuffer->GetDepthAttachmentID());
                        m_Scene->OnRenderDecals(gameVP, gameView, gameProj, gDepth,
                            m_GameFramebuffer->GetWidth(), m_GameFramebuffer->GetHeight());
                    }
                });

                // Game Pass 3: Sprites
                rg.AddPass("GameSprites", [&](VE::RGBuilder& b) {
                    b.Write(gameSceneColor);
                    b.SideEffect();
                }, [&](const VE::RGResources&) {
                    m_Scene->OnRenderSprites(gameVP);
                });

                // Game Pass 4: Particles
                rg.AddPass("GameParticles", [&](VE::RGBuilder& b) {
                    b.Write(gameSceneColor);
                    b.SideEffect();
                }, [&](const VE::RGResources&) {
                    m_Scene->OnRenderParticles(gameVP, gameCamPos);
                });

                // Game Pass 4: Runtime UI
                rg.AddPass("GameUI", [&](VE::RGBuilder& b) {
                    b.Write(gameSceneColor);
                    b.SideEffect();
                }, [&](const VE::RGResources&) {
                    uint32_t gw = m_GameFramebuffer->GetWidth();
                    uint32_t gh = m_GameFramebuffer->GetHeight();
                    double mx, my;
                    glfwGetCursorPos(GetWindow().GetNativeWindow(), &mx, &my);
                    bool mouseDown = glfwGetMouseButton(GetWindow().GetNativeWindow(), GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
                    m_Scene->OnRenderUI(gw, gh, (float)mx, (float)my, mouseDown);
                });

                // Game Pass 5: PostProcess
                rg.AddPass("GamePostProcess", [&](VE::RGBuilder& b) {
                    b.Read(gameSceneColor);
                    b.SideEffect();
                }, [&](const VE::RGResources&) {
                    m_GameFramebuffer->Unbind();
                    if (m_GameFramebuffer->IsMultisampled())
                        m_GameFramebuffer->Resolve();
                    auto ppSettings = BuildPostProcessSettings();
                    uint32_t gDepthTex = static_cast<uint32_t>(m_GameFramebuffer->GetDepthAttachmentID());
                    ppSettings.DepthTexture = gDepthTex;
                    if (ppSettings.MotionBlur.Enabled)
                        ppSettings.MotionBlur.DepthTexture = gDepthTex;

                    auto& gps = m_Scene->GetPipelineSettings();
                    if (gps.SSAOEnabled) {
                        VE::SSAOSettings ssaoS{ true, gps.SSAORadius, gps.SSAOBias,
                                                 gps.SSAOIntensity, gps.SSAOKernelSize };
                        ppSettings.SSAOTexture = m_GameSSAO.Compute(
                            gDepthTex, m_GameFramebuffer->GetWidth(), m_GameFramebuffer->GetHeight(),
                            gameProj, gameView, ssaoS);
                    }

                    // Compute SSR for game view
                    uint32_t gameTex = static_cast<uint32_t>(m_GameFramebuffer->GetColorAttachmentID());
                    if (gps.SSREnabled) {
                        VE::SSRSettings ssrS{ true, gps.SSRMaxSteps, gps.SSRStepSize,
                                               gps.SSRThickness, gps.SSRMaxDistance };
                        ppSettings.SSRTexture = m_GameSSR.Compute(
                            gameTex, gDepthTex,
                            m_GameFramebuffer->GetWidth(), m_GameFramebuffer->GetHeight(),
                            gameProj, gameView, ssrS);
                    }
                    m_GamePostProcessedTexture = m_GamePostProcessing.Apply(
                        gameTex, m_GameFramebuffer->GetWidth(), m_GameFramebuffer->GetHeight(), ppSettings);
                    if (m_GamePostProcessedTexture == gameTex)
                        m_GamePostProcessedTexture = 0;
                });
            }
        }

        rg.Compile();
        // Log graph structure once on first frame
        static bool s_DumpedOnce = false;
        if (!s_DumpedOnce) {
            VE_INFO("{}", rg.DumpGraph());
            s_DumpedOnce = true;
        }
        rg.Execute();
    }

    // Helper: build rotation matrix from a TransformComponent
    static glm::mat3 GetEntityRotation(const VE::TransformComponent& tc) {
        glm::mat4 rot(1.0f);
        rot = glm::rotate(rot, glm::radians(tc.Rotation[0]), glm::vec3(1, 0, 0));
        rot = glm::rotate(rot, glm::radians(tc.Rotation[1]), glm::vec3(0, 1, 0));
        rot = glm::rotate(rot, glm::radians(tc.Rotation[2]), glm::vec3(0, 0, 1));
        return glm::mat3(rot);
    }

    void OnImGuiRender() override {
        ImGuiIO& io = ImGui::GetIO();

        // ── Full-window dockspace ────────────────────────────────────
        {
            ImGuiWindowFlags dockFlags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking
                | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize
                | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
            ImGuiViewport* vp = ImGui::GetMainViewport();
            float toolbarH = 32.0f;
            ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + toolbarH));
            ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, vp->WorkSize.y - toolbarH));
            ImGui::SetNextWindowViewport(vp->ID);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::Begin("DockSpaceWindow", nullptr, dockFlags);
            ImGui::PopStyleVar(3);
            ImGui::DockSpace(ImGui::GetID("MainDockSpace"), ImVec2(0, 0), ImGuiDockNodeFlags_None);
            ImGui::End();
        }

        // ── Crash recovery popup ─────────────────────────────────────
        if (m_ShowRecoveryPopup) {
            ImGui::OpenPopup("Recover Auto-Save?");
            m_ShowRecoveryPopup = false;
        }
        if (ImGui::BeginPopupModal("Recover Auto-Save?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("An auto-save was found that is newer than the last saved scene.");
            ImGui::Text("Would you like to recover it?");
            ImGui::Spacing();
            if (ImGui::Button("Yes", ImVec2(120, 0))) {
                m_Scene = std::make_shared<VE::Scene>();
                ClearSelection();
                VE::SceneSerializer serializer(m_Scene);
                if (serializer.Deserialize("ProjectSettings/AutoSave.vscene")) {
                    VE_INFO("Recovered scene from auto-save");
                    m_SceneManager.UnloadAllScenes();
                    m_SceneManager.AddScene(m_Scene, "Recovered");
                    MarkDirty();
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("No", ImVec2(120, 0))) {
                std::filesystem::remove("ProjectSettings/AutoSave.vscene");
                VE_INFO("Discarded auto-save file");
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // ── Undo/Redo keyboard shortcuts ────────────────────────────
        if (!m_PlayMode) {
            // Update selected entity UUID for undo system
            if (m_SelectedEntity && m_SelectedEntity.HasComponent<VE::IDComponent>())
                m_CommandHistory.SetSelectedEntityUUID(static_cast<uint64_t>(m_SelectedEntity.GetComponent<VE::IDComponent>().ID));
            else
                m_CommandHistory.SetSelectedEntityUUID(0);

            bool ctrl = io.KeyCtrl;
            if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
                if (io.KeyShift)
                    m_CommandHistory.Redo();
                else
                    m_CommandHistory.Undo();
            }
            if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false))
                m_CommandHistory.Redo();
            if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D, false) && (m_SelectedEntity.GetHandle() != entt::null || !m_SelectedEntities.empty()))
                DuplicateSelectedEntity();
            if (ctrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
                auto& reg = m_Scene->GetRegistry();
                auto view = reg.view<VE::TagComponent>();
                ClearSelection();
                for (auto e : view) {
                    m_SelectedEntities.insert(e);
                    m_SelectedEntity = VE::Entity(e, &*m_Scene);
                }
            }

            // Copy/Cut/Paste (only when no text input is active)
            if (!io.WantTextInput) {
                if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C, false) && m_SelectedEntity.GetHandle() != entt::null)
                    CopySelectedEntity();
                if (ctrl && ImGui::IsKeyPressed(ImGuiKey_X, false) && m_SelectedEntity.GetHandle() != entt::null)
                    CutSelectedEntity();
                if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V, false))
                    PasteEntity();
                if (!ctrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_W, false))
                    m_GizmoTool = SceneGizmoTool::Translate;
                if (!ctrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_R, false))
                    m_GizmoTool = SceneGizmoTool::Rotate;
            }
        }

        // ── Gizmo drag continuation (runs even over ImGui panels) ────
        if (m_DraggingAxis != VE::GizmoAxis::None) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                auto& tc = m_SelectedEntity.GetComponent<VE::TransformComponent>();

                int axisIdx = (m_DraggingAxis == VE::GizmoAxis::X) ? 0
                            : (m_DraggingAxis == VE::GizmoAxis::Y) ? 1 : 2;

                if (m_GizmoTool == SceneGizmoTool::Rotate) {
                    float angleDelta = (io.MouseDelta.x - io.MouseDelta.y) * 0.35f;
                    tc.Rotation[axisIdx] += angleDelta;

                    for (auto selEntity : m_SelectedEntities) {
                        if (selEntity == m_SelectedEntity.GetHandle()) continue;
                        if (!m_Scene->GetRegistry().valid(selEntity)) continue;
                        if (!m_Scene->GetRegistry().any_of<VE::TransformComponent>(selEntity)) continue;
                        auto& otherTc = m_Scene->GetRegistry().get<VE::TransformComponent>(selEntity);
                        otherTc.Rotation[axisIdx] += angleDelta;
                    }
                } else {
                    // Project mouse in world space using the stored world position/rotation
                    float val = VE::GizmoRenderer::ProjectMouseOntoAxis(
                        m_DraggingAxis, m_DragWorldStartPos, io.MousePos.x, io.MousePos.y, m_DragWorldRot);
                    float worldDelta = val - m_DragOriginVal;

                    // Build world-space delta vector along the dragged axis
                    glm::vec3 worldAxis(0.0f);
                    worldAxis[axisIdx] = 1.0f;
                    worldAxis = m_DragWorldRot * worldAxis;
                    glm::vec3 worldDeltaVec = worldAxis * worldDelta;

                    // Convert world delta to parent-local delta
                    glm::vec3 localDelta = glm::vec3(m_DragParentInverse * glm::vec4(worldDeltaVec, 0.0f));
                    tc.Position[0] = m_DragStartPos[0] + localDelta.x;
                    tc.Position[1] = m_DragStartPos[1] + localDelta.y;
                    tc.Position[2] = m_DragStartPos[2] + localDelta.z;

                    // Batch move: apply same world delta to all other selected entities
                    for (auto selEntity : m_SelectedEntities) {
                        if (selEntity == m_SelectedEntity.GetHandle()) continue;
                        if (!m_Scene->GetRegistry().valid(selEntity)) continue;
                        if (!m_Scene->GetRegistry().any_of<VE::TransformComponent>(selEntity)) continue;
                        auto it = m_BatchDragStartPositions.find(selEntity);
                        if (it != m_BatchDragStartPositions.end()) {
                            auto& otherTc = m_Scene->GetRegistry().get<VE::TransformComponent>(selEntity);
                            otherTc.Position[0] = it->second[0] + localDelta.x;
                            otherTc.Position[1] = it->second[1] + localDelta.y;
                            otherTc.Position[2] = it->second[2] + localDelta.z;
                        }
                    }
                }
            } else {
                if (!m_PlayMode) m_CommandHistory.EndPropertyEdit();
                m_DraggingAxis = VE::GizmoAxis::None;
                m_BatchDragStartPositions.clear();
            }
        }

        // ── Camera + scene interaction (only when viewport is hovered) ───
        if (m_ViewportHovered && m_DraggingAxis == VE::GizmoAxis::None) {
            bool rightHeld = ImGui::IsMouseDown(ImGuiMouseButton_Right);
            if (io.MouseWheel != 0.0f && !rightHeld)
                m_Camera.OnMouseScroll(io.MouseWheel);
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
                m_Camera.OnMouseDrag(io.MouseDelta.x, io.MouseDelta.y);
            if (rightHeld) {
                if (ImGui::IsMouseDragging(ImGuiMouseButton_Right))
                    m_Camera.OnMouseRotate(io.MouseDelta.x, io.MouseDelta.y);
                // WASD fly-through while right-mouse is held
                m_Camera.OnFlyMove(
                    ImGui::IsKeyDown(ImGuiKey_W),
                    ImGui::IsKeyDown(ImGuiKey_S),
                    ImGui::IsKeyDown(ImGuiKey_A),
                    ImGui::IsKeyDown(ImGuiKey_D),
                    ImGui::IsKeyDown(ImGuiKey_E) || ImGui::IsKeyDown(ImGuiKey_Space),
                    ImGui::IsKeyDown(ImGuiKey_Q),
                    ImGui::IsKeyDown(ImGuiKey_LeftShift),
                    m_DeltaTime);
                // Scroll to adjust fly speed while right-mouse held
                if (io.MouseWheel != 0.0f) {
                    float speed = m_Camera.GetFlySpeed();
                    speed *= (1.0f + io.MouseWheel * 0.15f);
                    speed = std::clamp(speed, 0.5f, 100.0f);
                    m_Camera.SetFlySpeed(speed);
                }
            }

            // Focus camera on selected entity (F key) — zoom in and align to object's +Z
            if (ImGui::IsKeyPressed(ImGuiKey_F) && m_SelectedEntity && m_SelectedEntity.HasComponent<VE::TransformComponent>()) {
                glm::mat4 worldMat = m_Scene->GetWorldTransform(m_SelectedEntity.GetHandle());
                glm::vec3 pos = glm::vec3(worldMat[3]);
                // Extract world scale for sizing
                float scaleX = glm::length(glm::vec3(worldMat[0]));
                float scaleY = glm::length(glm::vec3(worldMat[1]));
                float scaleZ = glm::length(glm::vec3(worldMat[2]));
                float objectSize = std::max({scaleX, scaleY, scaleZ});
                if (m_Camera.GetMode() == VE::CameraMode::Perspective3D) {
                    m_Camera.SetFocalPoint(pos);
                    m_Camera.SetDistance(objectSize * 3.0f);
                    m_Camera.SetYaw(90.0f);
                    m_Camera.SetPitch(0.0f);
                } else {
                    m_Camera.SetPosition2D(glm::vec2(pos.x, pos.y));
                    m_Camera.SetZoom(1.0f / std::max(objectSize, 0.1f));
                }
            }

            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                VE::GizmoAxis axis = VE::GizmoAxis::None;
                bool canDrag = m_SelectedEntity && m_SelectedEntity.HasComponent<VE::TransformComponent>();
                if (canDrag && m_PlayMode && m_SelectedEntity.HasComponent<VE::RigidbodyComponent>()) {
                    auto& rb = m_SelectedEntity.GetComponent<VE::RigidbodyComponent>();
                    if (rb.Type == VE::BodyType::Dynamic)
                        canDrag = false;
                }
                if (canDrag) {
                    // Use world transform for gizmo hit testing (supports parented entities)
                    glm::mat4 worldMat = m_Scene->GetWorldTransform(m_SelectedEntity.GetHandle());
                    glm::vec3 worldPos = glm::vec3(worldMat[3]);
                    glm::mat3 worldRot = glm::mat3(worldMat);
                    worldRot[0] = glm::normalize(worldRot[0]);
                    worldRot[1] = glm::normalize(worldRot[1]);
                    worldRot[2] = glm::normalize(worldRot[2]);
                    if (m_GizmoTool == SceneGizmoTool::Rotate) {
                        axis = VE::GizmoRenderer::HitTestRotationGizmo(
                            worldPos, io.MousePos.x, io.MousePos.y, 12.0f, worldRot);
                    } else {
                        axis = VE::GizmoRenderer::HitTestTranslationGizmo(
                            worldPos, io.MousePos.x, io.MousePos.y, 12.0f, worldRot);
                    }
                }

                if (axis != VE::GizmoAxis::None) {
                    // Start gizmo drag (batch move will be applied for all selected)
                    if (!m_PlayMode)
                        m_CommandHistory.BeginPropertyEdit(
                            m_GizmoTool == SceneGizmoTool::Rotate ? "Rotate Entity" : "Move Entity");
                    m_DraggingAxis = axis;
                    auto& tc = m_SelectedEntity.GetComponent<VE::TransformComponent>();
                    m_DragStartPos = { tc.Position[0], tc.Position[1], tc.Position[2] };

                    // Compute world-space info for drag projection
                    glm::mat4 worldMat = m_Scene->GetWorldTransform(m_SelectedEntity.GetHandle());
                    m_DragWorldStartPos = glm::vec3(worldMat[3]);
                    m_DragWorldRot = glm::mat3(worldMat);
                    m_DragWorldRot[0] = glm::normalize(m_DragWorldRot[0]);
                    m_DragWorldRot[1] = glm::normalize(m_DragWorldRot[1]);
                    m_DragWorldRot[2] = glm::normalize(m_DragWorldRot[2]);

                    // Compute inverse of parent's world transform for local conversion
                    auto& rel = m_SelectedEntity.GetComponent<VE::RelationshipComponent>();
                    if (rel.Parent != entt::null)
                        m_DragParentInverse = glm::inverse(m_Scene->GetWorldTransform(rel.Parent));
                    else
                        m_DragParentInverse = glm::mat4(1.0f);

                    m_DragOriginVal = VE::GizmoRenderer::ProjectMouseOntoAxis(
                        axis, m_DragWorldStartPos, io.MousePos.x, io.MousePos.y, m_DragWorldRot);

                    // Capture start positions of all selected entities for batch move
                    m_BatchDragStartPositions.clear();
                    for (auto selEntity : m_SelectedEntities) {
                        if (m_Scene->GetRegistry().valid(selEntity) &&
                            m_Scene->GetRegistry().any_of<VE::TransformComponent>(selEntity)) {
                            auto& stc = m_Scene->GetRegistry().get<VE::TransformComponent>(selEntity);
                            m_BatchDragStartPositions[selEntity] = { stc.Position[0], stc.Position[1], stc.Position[2] };
                        }
                    }
                } else {
                    // Click to select entity or start box selection
                    VE::Entity hitEntity = HitTestCameraGizmoIcons(io.MousePos.x, io.MousePos.y);
                    if (!hitEntity)
                        hitEntity = HitTestEntities(io.MousePos.x, io.MousePos.y);
                    if (hitEntity) {
                        if (io.KeyCtrl) {
                            ToggleEntitySelection(hitEntity.GetHandle());
                        } else {
                            SelectEntityOnly(hitEntity);
                        }
                    } else {
                        // Clicked on empty space: start box selection
                        if (!io.KeyCtrl)
                            ClearSelection();
                        m_EntityBoxSelecting = true;
                        m_EntityBoxSelectStart = io.MousePos;
                        m_EntityBoxSelectEnd = io.MousePos;
                    }
                }
            }

            // Box selection drag continuation
            if (m_EntityBoxSelecting) {
                m_EntityBoxSelectEnd = io.MousePos;
                if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    // Finish box select
                    m_EntityBoxSelecting = false;
                    float dx = m_EntityBoxSelectEnd.x - m_EntityBoxSelectStart.x;
                    float dy = m_EntityBoxSelectEnd.y - m_EntityBoxSelectStart.y;
                    if (dx * dx + dy * dy > 25.0f) { // Only box-select if dragged at least 5px
                        PerformBoxSelect(m_EntityBoxSelectStart, m_EntityBoxSelectEnd, io.KeyCtrl);
                    }
                }
            }
        }

        DrawMainMenuBar();
        DrawToolbar();
        DrawViewportPanel();
        DrawHierarchyPanel();
        DrawInspectorPanel();
        DrawSceneInfoPanel();
        DrawPipelineSettingsPanel();
        DrawRenderDebuggerPanel();
        DrawScriptingPanel();
        DrawContentBrowserPanel();
        DrawProjectSettingsPanel();
        DrawGameViewPanel();
        DrawInputSettingsPanel();
        DrawBuildPanel();
        DrawMaterialEditorPanel();

        DrawProfilerPanel();
        DrawScriptBuildStatus();

        // Render all editor plugin UIs (pass viewport bounds for overlay positioning)
        VE::PluginEngine::SetViewportBounds(m_SceneVpX, m_SceneVpY, m_SceneVpW, m_SceneVpH);
        VE::PluginEngine::RenderAllPluginUI();

        DrawSceneManagerPanel();
        DrawTransitionOverlay();
        MaybeAutoExportRenderDiagnostics();

    }

    void OnRendererReloaded() override {
        if (VE::MeshLibrary::GetTriangle()) {
            ClearEntityGPUResources();
            VE::MeshLibrary::Shutdown();
            VE::MeshImporter::ClearCache();
            VE::UIRenderer::Shutdown();
            m_Framebuffer.reset();
            m_GameFramebuffer.reset();
            m_PostProcessing.Shutdown();
            m_GamePostProcessing.Shutdown();
            m_PostProcessedTexture = 0;
            m_GamePostProcessedTexture = 0;
        } else {
            VE::MeshLibrary::Init();
            VE::MeshImporter::ReuploadCache();
            VE::UIRenderer::Init();
            RestoreEntityGPUResources();
            VE::FramebufferSpec fbSpec;
            fbSpec.Width = 1280;
            fbSpec.Height = 720;
            fbSpec.HDR = true;
            m_Framebuffer = VE::Framebuffer::Create(fbSpec);
            m_GameFramebuffer = VE::Framebuffer::Create(fbSpec);
        }
        m_ThumbnailCache.Clear();
    }

private:
    bool IsLauncherScene() const {
        if (m_CurrentScenePath.empty())
            return false;

        return std::filesystem::path(m_CurrentScenePath).stem().generic_string() == "launcher";
    }

    VE::Entity FindEntityByName(const std::string& name) {
        auto view = m_Scene->GetAllEntitiesWith<VE::TagComponent>();
        for (auto entityID : view) {
            auto& tag = view.get<VE::TagComponent>(entityID);
            if (tag.Tag == name)
                return VE::Entity(entityID, &*m_Scene);
        }
        return {};
    }

    VE::Entity FindMainCameraEntity() {
        auto view = m_Scene->GetAllEntitiesWith<VE::TagComponent, VE::CameraComponent>();
        for (auto entityID : view) {
            auto& tag = view.get<VE::TagComponent>(entityID);
            if (tag.GameObjectTag == "MainCamera")
                return VE::Entity(entityID, &*m_Scene);
        }
        return FindEntityByName("Camera");
    }

    VE::Entity CreateLauncherPrimitive(const std::string& name,
                                       const std::shared_ptr<VE::VertexArray>& mesh,
                                       const glm::vec3& position,
                                       const glm::vec3& scale,
                                       const std::array<float, 4>& color,
                                       bool castShadows,
                                       VE::Entity parent = {}) {
        auto entity = m_Scene->CreateEntity(name);
        auto& tc = entity.GetComponent<VE::TransformComponent>();
        tc.Position = { position.x, position.y, position.z };
        tc.Scale = { scale.x, scale.y, scale.z };

        auto& mr = entity.AddComponent<VE::MeshRendererComponent>();
        mr.Mesh = mesh;
        mr.Mat = VE::MaterialLibrary::Get("Lit");
        mr.Color = color;
        mr.CastShadows = castShadows;

        for (int i = 0; i < VE::MeshLibrary::GetMeshCount(); ++i) {
            if (mesh == VE::MeshLibrary::GetMeshByIndex(i)) {
                mr.LocalBounds = VE::MeshLibrary::GetMeshAABB(i);
                break;
            }
        }

        if (parent)
            m_Scene->SetParent(entity.GetHandle(), parent.GetHandle());

        return entity;
    }

    std::string ResolveLauncherAssetPath(const std::string& relativePath) const {
        std::filesystem::path runtimePath(relativePath);
        if (std::filesystem::exists(runtimePath))
            return runtimePath.generic_string();

        std::filesystem::path sourcePath = std::filesystem::path(VE_PROJECT_ROOT) / relativePath;
        if (std::filesystem::exists(sourcePath))
            return sourcePath.generic_string();

        return relativePath;
    }

    std::shared_ptr<VE::Texture2D> GetLauncherTexture(const std::string& resolvedTexturePath) {
        static std::unordered_map<std::string, std::shared_ptr<VE::Texture2D>> textureCache;

        auto it = textureCache.find(resolvedTexturePath);
        if (it != textureCache.end())
            return it->second;

        auto texture = VE::Texture2D::Create(resolvedTexturePath);
        textureCache[resolvedTexturePath] = texture;
        return texture;
    }

    void ApplyLauncherMainTexture(VE::MeshRendererComponent& mr, const std::string& texturePath) {
        const std::string resolvedTexturePath = ResolveLauncherAssetPath(texturePath);

        for (auto& ov : mr.MaterialOverrides) {
            if (ov.Name != "u_MainTex")
                continue;

            ov.Type = VE::MaterialPropertyType::Texture2D;
            ov.FlagName = "u_HasMainTex";
            if (ov.TexturePath == resolvedTexturePath && ov.TextureRef)
                return;

            ov.TexturePath = resolvedTexturePath;
            ov.TextureRef = GetLauncherTexture(resolvedTexturePath);
            return;
        }

        VE::MaterialProperty mainTex;
        mainTex.Name = "u_MainTex";
        mainTex.DisplayName = "Albedo";
        mainTex.Type = VE::MaterialPropertyType::Texture2D;
        mainTex.TexturePath = resolvedTexturePath;
        mainTex.TextureRef = GetLauncherTexture(resolvedTexturePath);
        mainTex.FlagName = "u_HasMainTex";
        mr.MaterialOverrides.push_back(std::move(mainTex));
    }

    void EnsureLauncherImportedTextures() {
        static constexpr const char* kKenneyTrainKit = "KenneyTrainKit";
        static constexpr const char* kKenneyColorMap = "Assets/Models/KenneyTrainKit/colormap.png";

        auto view = m_Scene->GetAllEntitiesWith<VE::MeshRendererComponent>();
        for (auto entityID : view) {
            auto& mr = view.get<VE::MeshRendererComponent>(entityID);
            if (mr.MeshSourcePath.find(kKenneyTrainKit) == std::string::npos)
                continue;

            mr.Color = { 1.0f, 1.0f, 1.0f, 1.0f };
            ApplyLauncherMainTexture(mr, kKenneyColorMap);
        }
    }

    void EnsureLauncherWater() {
        auto waterEntity = FindEntityByName("HPWaterOcean");
        if (!waterEntity) {
            waterEntity = m_Scene->CreateEntity("HPWaterOcean");
            auto& tag = waterEntity.GetComponent<VE::TagComponent>();
            tag.Layer = 4;

            auto& tc = waterEntity.GetComponent<VE::TransformComponent>();
            tc.Position = { 0.0f, 0.0f, 72.0f };
            tc.Scale = { 1.0f, 1.0f, 1.0f };

            auto& water = waterEntity.AddComponent<VE::HPWaterComponent>();
            water.WorldSizeX = 220.0f;
            water.WorldSizeZ = 360.0f;
            water.BaseHeight = -0.42f;
            water.Resolution = 128;
            water.HeightScale = 0.45f;
            water.WaveSpeed = 12.0f;
            water.Damping = 0.020f;
            water.EdgeAbsorptionWidth = 0.14f;
            water.SpectrumWaves = true;
            water.SpectrumAmplitude = 0.85f;
            water.SpectrumWindAngle = 28.0f;
            water.SpectrumWindSpeed = 13.0f;
            water.SpectrumDirectionalSpread = 0.76f;
            water.SpectrumSwell = 0.62f;
            water.SpectrumShortWaveFade = 0.34f;
            water.SpectrumTimeScale = 1.35f;
            water.SpectrumNormalStrength = 1.35f;
            water.Choppiness = 0.45f;
            water.AutoImpulse = true;
            water.ImpulseRadius = 8.5f;
            water.ImpulseStrength = 0.060f;
            water.ScatterColor = { 0.035f, 0.28f, 0.36f };
            water.AbsorptionColor = { 0.020f, 0.060f, 0.090f };
            water.FoamIntensity = 0.18f;
            water.IndirectLightStrength = 1.0f;
            water.SpecularFGDStrength = 1.0f;
            water.GGXEnergyCompensation = 1.0f;
            if (m_HPWaterSSRDiagnostics) {
                water.HeightScale = 0.0f;
                water.SpectrumAmplitude = 0.0f;
                water.SpectrumNormalStrength = 0.0f;
                water.Choppiness = 0.0f;
                water.Roughness = 0.02f;
                water.AutoImpulse = false;
                water.FoamIntensity = 0.0f;
            }

            auto& mr = waterEntity.AddComponent<VE::MeshRendererComponent>();
            mr.Mat = VE::MaterialLibrary::Get("Water");
            mr.Color = { 1.0f, 1.0f, 1.0f, 0.82f };
            mr.CastShadows = false;
        } else {
            if (!waterEntity.HasComponent<VE::HPWaterComponent>())
                waterEntity.AddComponent<VE::HPWaterComponent>();
            auto& water = waterEntity.GetComponent<VE::HPWaterComponent>();
            water.IndirectLightStrength = std::clamp(water.IndirectLightStrength, 0.0f, 4.0f);
            water.SpecularFGDStrength = std::clamp(water.SpecularFGDStrength, 0.0f, 1.0f);
            water.GGXEnergyCompensation = std::clamp(water.GGXEnergyCompensation, 0.0f, 2.0f);
            water.SpectrumWindSpeed = std::clamp(water.SpectrumWindSpeed, 0.0f, 80.0f);
            water.SpectrumDirectionalSpread = std::clamp(water.SpectrumDirectionalSpread, 0.0f, 1.0f);
            water.SpectrumSwell = std::clamp(water.SpectrumSwell, 0.0f, 1.0f);
            water.SpectrumShortWaveFade = std::clamp(water.SpectrumShortWaveFade, 0.0f, 2.0f);
            water.SpectrumNormalStrength = std::clamp(water.SpectrumNormalStrength, 0.0f, 4.0f);
            if (m_HPWaterSSRDiagnostics) {
                water.HeightScale = 0.0f;
                water.SpectrumAmplitude = 0.0f;
                water.SpectrumNormalStrength = 0.0f;
                water.Choppiness = 0.0f;
                water.Roughness = 0.02f;
                water.AutoImpulse = false;
                water.FoamIntensity = 0.0f;
            }
            auto& mr = waterEntity.HasComponent<VE::MeshRendererComponent>()
                ? waterEntity.GetComponent<VE::MeshRendererComponent>()
                : waterEntity.AddComponent<VE::MeshRendererComponent>();
            mr.Mat = VE::MaterialLibrary::Get("Water");
            mr.CastShadows = false;
        }
    }

    void EnsureLauncherReflectionProbe() {
        auto ensureProbe = [&](const std::string& name,
                               const std::array<float, 3>& position,
                               const std::array<float, 3>& boxSize) {
            auto probeEntity = FindEntityByName(name);
            if (!probeEntity)
                probeEntity = m_Scene->CreateEntity(name);

            auto& tc = probeEntity.GetComponent<VE::TransformComponent>();
            tc.Position = position;
            tc.Scale = { 1.0f, 1.0f, 1.0f };

            auto& probe = probeEntity.HasComponent<VE::ReflectionProbeComponent>()
                ? probeEntity.GetComponent<VE::ReflectionProbeComponent>()
                : probeEntity.AddComponent<VE::ReflectionProbeComponent>();

            probe.Resolution = 128;
            probe.BoxSize = boxSize;
            probe.BakeOnLoad = false;

            if (!probe._Probe || !probe._Probe->IsBaked() || probe._Probe->GetCubemapID() == 0)
                m_Scene->BakeReflectionProbe(probeEntity.GetHandle());
        };

        ensureProbe("HPWater Reflection Probe", { 0.0f, 2.5f, 22.0f }, { 80.0f, 24.0f, 80.0f });
        ensureProbe("HPWater Reflection Probe Secondary", { 32.0f, 2.5f, 22.0f }, { 80.0f, 24.0f, 80.0f });
    }

    void EnsureLauncherHPWaterAreaLight() {
        auto areaLightEntity = FindEntityByName("HPWater Area Light");
        if (!areaLightEntity)
            areaLightEntity = m_Scene->CreateEntity("HPWater Area Light");

        auto& tag = areaLightEntity.GetComponent<VE::TagComponent>();
        tag.Layer = 4;

        auto& tc = areaLightEntity.GetComponent<VE::TransformComponent>();
        tc.Position = { -8.0f, 5.0f, 28.0f };
        tc.Rotation = EulerFromForwardDirection(glm::vec3(0.35f, -0.75f, 0.55f));
        tc.Scale = { 1.0f, 1.0f, 1.0f };

        auto& area = areaLightEntity.HasComponent<VE::AreaLightComponent>()
            ? areaLightEntity.GetComponent<VE::AreaLightComponent>()
            : areaLightEntity.AddComponent<VE::AreaLightComponent>();
        area.Color = { 0.55f, 0.82f, 1.0f };
        area.Intensity = 2.4f;
        area.Range = 42.0f;
        area.Width = 14.0f;
        area.Height = 7.0f;
    }

    void UpdateHPWaterMotionDiagnosticObject(float deltaTime) {
        if (!m_HPWaterMotionDiagnostics || !m_PlayMode)
            return;

        auto entity = FindEntityByName("HPWater Motion Diagnostic Object");
        if (!entity) {
            entity = CreateLauncherPrimitive("HPWater Motion Diagnostic Object",
                VE::MeshLibrary::GetCube(),
                { -3.0f, 0.75f, 8.0f },
                { 1.6f, 1.6f, 1.6f },
                { 0.95f, 0.45f, 0.12f, 1.0f },
                true);
        }

        if (!entity || !entity.HasComponent<VE::TransformComponent>())
            return;

        auto& tag = entity.GetComponent<VE::TagComponent>();
        tag.Layer = 0;
        tag.GameObjectTag = "EditorOnly";

        auto& tc = entity.GetComponent<VE::TransformComponent>();
        m_HPWaterMotionDiagnosticTime += std::max(deltaTime, 0.0f);
        tc.Position[0] = -3.0f + std::sin(m_HPWaterMotionDiagnosticTime * 1.7f) * 2.0f;
        tc.Position[1] = 0.75f;
        tc.Position[2] = 8.0f + m_HPWaterMotionDiagnosticTime * 3.0f;
    }

    void EnsureHPWaterFluidFilterDiagnosticObjects(float deltaTime) {
        if (!m_HPWaterFluidFilterDiagnostics || !m_PlayMode)
            return;

        m_HPWaterFluidFilterDiagnosticTime += std::max(deltaTime, 0.0f);

        auto ensureDiagnosticCube = [&](const std::string& name,
                                        const glm::vec3& position,
                                        const glm::vec3& scale,
                                        const std::array<float, 4>& color,
                                        int layer) {
            auto entity = FindEntityByName(name);
            if (!entity) {
                entity = CreateLauncherPrimitive(name,
                    VE::MeshLibrary::GetCube(),
                    position,
                    scale,
                    color,
                    true);
            }

            if (!entity || !entity.HasComponent<VE::TransformComponent>())
                return VE::Entity();

            auto& tag = entity.GetComponent<VE::TagComponent>();
            tag.Layer = layer;
            tag.GameObjectTag = "EditorOnly";
            tag.Active = true;

            auto& tc = entity.GetComponent<VE::TransformComponent>();
            tc.Position = { position.x, position.y, position.z };
            tc.Scale = { scale.x, scale.y, scale.z };

            auto& mr = entity.HasComponent<VE::MeshRendererComponent>()
                ? entity.GetComponent<VE::MeshRendererComponent>()
                : entity.AddComponent<VE::MeshRendererComponent>();
            mr.Mat = VE::MaterialLibrary::Get("Lit");
            mr.Color = color;
            mr.CastShadows = false;
            return entity;
        };

        ensureDiagnosticCube("HPWater Fluid Water Layer Diagnostic",
            { -4.0f, 0.35f, 12.0f },
            { 1.0f, 0.25f, 1.0f },
            { 0.0f, 0.55f, 1.0f, 1.0f },
            4);
        ensureDiagnosticCube("HPWater Fluid Opaque Diagnostic",
            { 4.0f, 0.45f, 12.0f },
            { 1.0f, 0.9f, 1.0f },
            { 1.0f, 0.65f, 0.1f, 1.0f },
            0);
        ensureDiagnosticCube("HPWater Fluid Transparent Diagnostic",
            { 0.0f, 0.7f, 16.0f },
            { 1.2f, 1.2f, 1.2f },
            { 1.0f, 0.2f, 0.9f, 0.35f },
            0);

        const float movingX = 6.0f + std::sin(m_HPWaterFluidFilterDiagnosticTime * 2.5f) * 2.25f;
        const float movingZ = 24.0f + std::cos(m_HPWaterFluidFilterDiagnosticTime * 1.7f) * 1.25f;
        ensureDiagnosticCube("HPWater Fluid Moving Opaque Diagnostic",
            { movingX, 0.55f, movingZ },
            { 1.35f, 1.1f, 1.35f },
            { 0.2f, 1.0f, 0.35f, 1.0f },
            0);
    }

    void EnsureHPWaterSSRDiagnosticObjects() {
        if (!m_HPWaterSSRDiagnostics || !m_PlayMode)
            return;

        glm::vec3 anchor{ 0.0f, 0.0f, 18.0f };
        auto train = FindEntityByName("LauncherTrain");
        if (train && train.HasComponent<VE::TransformComponent>()) {
            const auto& trainTc = train.GetComponent<VE::TransformComponent>();
            anchor.z = trainTc.Position[2] + 36.0f;
        }

        auto entity = FindEntityByName("HPWater SSR Diagnostic Reflector");
        if (!entity) {
            entity = CreateLauncherPrimitive("HPWater SSR Diagnostic Reflector",
                VE::MeshLibrary::GetCube(),
                { anchor.x, 5.5f, anchor.z },
                { 48.0f, 12.0f, 0.6f },
                { 0.05f, 0.95f, 1.0f, 1.0f },
                false);
        }

        if (!entity || !entity.HasComponent<VE::TransformComponent>())
            return;

        auto& tag = entity.GetComponent<VE::TagComponent>();
        tag.Layer = 0;
        tag.GameObjectTag = "EditorOnly";
        tag.Active = true;

        auto& tc = entity.GetComponent<VE::TransformComponent>();
        tc.Position = { anchor.x, 5.5f, anchor.z };
        tc.Scale = { 48.0f, 12.0f, 0.6f };

        auto& mr = entity.HasComponent<VE::MeshRendererComponent>()
            ? entity.GetComponent<VE::MeshRendererComponent>()
            : entity.AddComponent<VE::MeshRendererComponent>();
        mr.Mesh = VE::MeshLibrary::GetCube();
        mr.Mat = VE::MaterialLibrary::Get("Lit");
        mr.Color = { 0.05f, 0.95f, 1.0f, 1.0f };
        mr.CastShadows = false;
    }

    VE::Entity CreateLauncherImportedMesh(const std::string& name,
                                          const std::string& meshPath,
                                          const glm::vec3& position,
                                          const glm::vec3& scale,
                                          const std::array<float, 4>& color,
                                          bool castShadows,
                                          VE::Entity parent = {}) {
        const std::string resolvedPath = ResolveLauncherAssetPath(meshPath);
        auto meshAsset = VE::MeshImporter::GetOrLoad(resolvedPath);
        if (!meshAsset || !meshAsset->VAO)
            return {};

        auto entity = m_Scene->CreateEntity(name);
        auto& tc = entity.GetComponent<VE::TransformComponent>();
        tc.Position = { position.x, position.y, position.z };
        tc.Scale = { scale.x, scale.y, scale.z };

        auto& mr = entity.AddComponent<VE::MeshRendererComponent>();
        mr.Mesh = meshAsset->VAO;
        mr.Mat = VE::MaterialLibrary::Get("Lit");
        mr.Color = color;
        mr.MeshSourcePath = resolvedPath;
        mr.CastShadows = castShadows;
        mr.LocalBounds = meshAsset->BoundingBox;
        ApplyLauncherMainTexture(mr, "Assets/Models/KenneyTrainKit/colormap.png");

        if (parent)
            m_Scene->SetParent(entity.GetHandle(), parent.GetHandle());

        return entity;
    }

    bool LauncherRailSegmentExists(int segmentIndex) {
        return static_cast<bool>(FindEntityByName("RailSegment_" + std::to_string(segmentIndex)));
    }

    void CreateLauncherRailSegment(int segmentIndex, float segmentLength) {
        const float centerZ = static_cast<float>(segmentIndex) * segmentLength;
        auto root = m_Scene->CreateEntity("RailSegment_" + std::to_string(segmentIndex));
        auto& rootTag = root.GetComponent<VE::TagComponent>();
        rootTag.GameObjectTag = "EditorOnly";

        auto track = CreateLauncherImportedMesh("RailroadStraight_" + std::to_string(segmentIndex),
            "Assets/Models/KenneyTrainKit/railroad-straight.fbx",
            { 0.0f, 0.0f, centerZ },
            { 1.0f, 1.0f, 2.35f },
            { 1.0f, 1.0f, 1.0f, 1.0f },
            true,
            root);

        if (!track) {
            CreateLauncherPrimitive("RailBed_" + std::to_string(segmentIndex),
                VE::MeshLibrary::GetPlane(),
                { 0.0f, 0.0f, centerZ },
                { 6.0f, 1.0f, segmentLength },
                { 0.28f, 0.27f, 0.24f, 1.0f },
                false,
                root);

            CreateLauncherPrimitive("RailLeft_" + std::to_string(segmentIndex),
                VE::MeshLibrary::GetCube(),
                { -0.85f, 0.12f, centerZ },
                { 0.12f, 0.12f, segmentLength * 0.96f },
                { 0.42f, 0.42f, 0.44f, 1.0f },
                true,
                root);

            CreateLauncherPrimitive("RailRight_" + std::to_string(segmentIndex),
                VE::MeshLibrary::GetCube(),
                { 0.85f, 0.12f, centerZ },
                { 0.12f, 0.12f, segmentLength * 0.96f },
                { 0.42f, 0.42f, 0.44f, 1.0f },
                true,
                root);

            for (int i = 0; i < 6; ++i) {
                const float offset = -segmentLength * 0.42f + static_cast<float>(i) * (segmentLength * 0.84f / 5.0f);
                CreateLauncherPrimitive("RailTie_" + std::to_string(segmentIndex) + "_" + std::to_string(i),
                    VE::MeshLibrary::GetCube(),
                    { 0.0f, 0.05f, centerZ + offset },
                    { 2.35f, 0.1f, 0.28f },
                    { 0.22f, 0.13f, 0.075f, 1.0f },
                    true,
                    root);
            }
        }
    }

    void DestroyLauncherRailSegmentsOutside(int minSegment, int maxSegment) {
        std::vector<VE::Entity> toDestroy;
        auto view = m_Scene->GetAllEntitiesWith<VE::TagComponent>();
        for (auto entityID : view) {
            auto& tag = view.get<VE::TagComponent>(entityID);
            const std::string prefix = "RailSegment_";
            if (tag.Tag.rfind(prefix, 0) != 0)
                continue;

            int segmentIndex = 0;
            try {
                segmentIndex = std::stoi(tag.Tag.substr(prefix.size()));
            } catch (...) {
                continue;
            }

            if (segmentIndex < minSegment || segmentIndex > maxSegment)
                toDestroy.emplace_back(entityID, &*m_Scene);
        }

        for (auto entity : toDestroy)
            m_Scene->DestroyEntity(entity);
    }

    void UpdateLauncherPrototype(float deltaTime) {
        if (!IsLauncherScene())
            return;

        EnsureLauncherImportedTextures();
        EnsureLauncherWater();
        EnsureLauncherReflectionProbe();
        EnsureLauncherHPWaterAreaLight();

        if (!m_PlayMode)
            return;

        UpdateHPWaterMotionDiagnosticObject(deltaTime);
        EnsureHPWaterFluidFilterDiagnosticObjects(deltaTime);
        EnsureHPWaterSSRDiagnosticObjects();

        auto train = FindEntityByName("LauncherTrain");
        if (!train || !train.HasComponent<VE::TransformComponent>())
            return;

        constexpr float kTrainSpeed = 7.5f;
        constexpr float kSegmentLength = 12.0f;
        constexpr int kSegmentsBehind = 3;
        constexpr int kSegmentsAhead = 9;

        auto& trainTc = train.GetComponent<VE::TransformComponent>();
        trainTc.Position[2] += kTrainSpeed * deltaTime;

        const int currentSegment = static_cast<int>(std::floor(trainTc.Position[2] / kSegmentLength));
        const int minSegment = currentSegment - kSegmentsBehind;
        const int maxSegment = currentSegment + kSegmentsAhead;

        for (int segment = minSegment; segment <= maxSegment; ++segment) {
            if (!LauncherRailSegmentExists(segment))
                CreateLauncherRailSegment(segment, kSegmentLength);
        }
        DestroyLauncherRailSegmentsOutside(minSegment, maxSegment);

        auto camera = FindMainCameraEntity();
        if (camera && camera.HasComponent<VE::TransformComponent>()) {
            auto& cameraTc = camera.GetComponent<VE::TransformComponent>();
            cameraTc.Position = { trainTc.Position[0], trainTc.Position[1] + 6.0f, trainTc.Position[2] - 15.0f };
        }
    }

    // ── Scene picking ─────────────────────────────────────────────────

    std::string NormalizeMeshAssetPath(const std::string& path) const {
        if (path.empty())
            return {};

        std::filesystem::path fsPath(path);
        if (!fsPath.is_absolute() && !std::filesystem::exists(fsPath))
            fsPath = std::filesystem::path(m_AssetDatabase.GetAssetsRoot()) / path;

        std::error_code ec;
        auto canonical = std::filesystem::weakly_canonical(fsPath, ec);
        if (!ec)
            return canonical.generic_string();

        return fsPath.lexically_normal().generic_string();
    }

    void RebindSceneMeshAsset(const std::string& changedPath, const std::shared_ptr<VE::MeshAsset>& meshAsset) {
        if (!meshAsset || !meshAsset->VAO)
            return;

        const std::string changedNorm = NormalizeMeshAssetPath(changedPath);
        auto view = m_Scene->GetAllEntitiesWith<VE::MeshRendererComponent>();
        for (auto entityID : view) {
            auto& mr = view.get<VE::MeshRendererComponent>(entityID);
            if (mr.MeshSourcePath.empty())
                continue;

            if (NormalizeMeshAssetPath(mr.MeshSourcePath) != changedNorm)
                continue;

            mr.Mesh = meshAsset->VAO;
            mr.LocalBounds = meshAsset->BoundingBox;
        }
    }

    // Ray-AABB intersection (slab method). Returns hit distance or -1.
    static float RayAABBIntersect(const glm::vec3& rayOrigin, const glm::vec3& rayDir,
                                   const glm::vec3& boxMin, const glm::vec3& boxMax) {
        glm::vec3 invDir = 1.0f / rayDir;
        glm::vec3 t1 = (boxMin - rayOrigin) * invDir;
        glm::vec3 t2 = (boxMax - rayOrigin) * invDir;
        glm::vec3 tMin = glm::min(t1, t2);
        glm::vec3 tMax = glm::max(t1, t2);
        float tNear = std::max({ tMin.x, tMin.y, tMin.z });
        float tFar  = std::min({ tMax.x, tMax.y, tMax.z });
        if (tNear > tFar || tFar < 0.0f) return -1.0f;
        return tNear >= 0.0f ? tNear : tFar;
    }

    // Get local-space AABB for an entity's mesh
    VE::AABB GetEntityLocalAABB(const VE::MeshRendererComponent& mr) {
        // Check imported meshes first
        if (!mr.MeshSourcePath.empty()) {
            auto meshAsset = VE::MeshImporter::GetOrLoad(mr.MeshSourcePath);
            if (meshAsset && meshAsset->BoundingBox.Valid())
                return meshAsset->BoundingBox;
        }
        // Built-in mesh: find index by comparing VAO pointer
        for (int i = 0; i < VE::MeshLibrary::GetMeshCount(); i++) {
            if (mr.Mesh == VE::MeshLibrary::GetMeshByIndex(i))
                return VE::MeshLibrary::GetMeshAABB(i);
        }
        // Fallback: unit cube
        return VE::AABB{ glm::vec3(-0.5f), glm::vec3(0.5f) };
    }

    VE::Entity HitTestEntities(float screenX, float screenY) {
        // Convert screen coords to NDC
        float ndcX = ((screenX - m_SceneVpX) / m_SceneVpW) * 2.0f - 1.0f;
        float ndcY = 1.0f - ((screenY - m_SceneVpY) / m_SceneVpH) * 2.0f;

        // Unproject to world-space ray
        glm::mat4 invVP = glm::inverse(m_FrameVP);
        glm::vec4 nearH = invVP * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
        glm::vec4 farH  = invVP * glm::vec4(ndcX, ndcY,  1.0f, 1.0f);
        glm::vec3 nearW = glm::vec3(nearH) / nearH.w;
        glm::vec3 farW  = glm::vec3(farH)  / farH.w;
        glm::vec3 rayDir = glm::normalize(farW - nearW);

        VE::Entity best;
        float bestDist = std::numeric_limits<float>::max();

        auto view = m_Scene->GetAllEntitiesWith<VE::TransformComponent, VE::MeshRendererComponent>();
        for (auto entityID : view) {
            auto& mr = view.get<VE::MeshRendererComponent>(entityID);
            VE::AABB localBox = GetEntityLocalAABB(mr);

            // Transform ray into local space (test against local AABB = OBB test)
            glm::mat4 worldMat = m_Scene->GetWorldTransform(entityID);
            glm::mat4 invWorld = glm::inverse(worldMat);
            glm::vec3 localOrigin = glm::vec3(invWorld * glm::vec4(nearW, 1.0f));
            glm::vec3 localDir    = glm::normalize(glm::vec3(invWorld * glm::vec4(rayDir, 0.0f)));

            float t = RayAABBIntersect(localOrigin, localDir, localBox.Min, localBox.Max);
            if (t >= 0.0f && t < bestDist) {
                bestDist = t;
                best = VE::Entity(entityID, &*m_Scene);
            }
        }
        return best;
    }

    // ── Scene operations ──────────────────────────────────────────────

    VE::Entity HitTestCameraGizmoIcons(float screenX, float screenY) {
        if (!m_GizmosEnabled)
            return {};

        constexpr float kHitHalfWidth = 42.0f;
        constexpr float kHitHalfHeight = 39.0f;

        VE::Entity best;
        float bestScore = std::numeric_limits<float>::max();
        auto view = m_Scene->GetAllEntitiesWith<VE::TransformComponent, VE::CameraComponent>();
        for (auto entityID : view) {
            glm::vec3 worldPos = glm::vec3(m_Scene->GetWorldTransform(entityID)[3]);
            glm::vec4 clip = m_FrameVP * glm::vec4(worldPos, 1.0f);
            if (clip.w <= 0.01f)
                continue;

            glm::vec3 ndc = glm::vec3(clip) / clip.w;
            if (ndc.x < -1.2f || ndc.x > 1.2f ||
                ndc.y < -1.2f || ndc.y > 1.2f ||
                ndc.z < -1.2f || ndc.z > 1.2f)
                continue;

            float iconX = m_SceneVpX + (ndc.x * 0.5f + 0.5f) * m_SceneVpW;
            float iconY = m_SceneVpY + (1.0f - (ndc.y * 0.5f + 0.5f)) * m_SceneVpH;
            float dx = std::abs(screenX - iconX);
            float dy = std::abs(screenY - iconY);
            if (dx > kHitHalfWidth || dy > kHitHalfHeight)
                continue;

            float score = dx * dx + dy * dy;
            if (score < bestScore) {
                bestScore = score;
                best = VE::Entity(entityID, &*m_Scene);
            }
        }

        return best;
    }

    void CreateAssetFile(const std::string& dir, const std::string& baseName, const std::string& ext) {
        std::string assetsRoot = m_AssetDatabase.GetAssetsRoot();
        std::string relDir = dir;

        // Find a unique name (scripts use no spaces so class name stays valid C++)
        std::string sep = (ext == ".cpp") ? "" : " ";
        std::string name = baseName;
        std::string relPath = relDir.empty() ? name + ext : relDir + "/" + name + ext;
        std::string absPath = (std::filesystem::path(assetsRoot) / relPath).generic_string();
        int counter = 1;
        while (std::filesystem::exists(absPath)) {
            name = baseName + sep + std::to_string(counter++);
            relPath = relDir.empty() ? name + ext : relDir + "/" + name + ext;
            absPath = (std::filesystem::path(assetsRoot) / relPath).generic_string();
        }

        if (ext == ".vmat") {
            // Create a default material file with properties from ShaderLab metadata
            auto mat = VE::Material::Create(name, VE::MeshLibrary::GetLitShader());
            mat->SetLit(true);
            mat->PopulateFromShader();
            mat->Save(absPath);
            VE::MaterialLibrary::Register(mat);
        } else if (ext == ".cpp") {
            // Create a clean script template (no DLL exports — handled by ScriptRegistry.gen.cpp)
            std::ofstream fout(absPath);
            fout << "#include <VibeEngine/Scripting/NativeScript.h>\n"
                 << "using namespace VE;\n\n"
                 << "class " << name << " : public NativeScript {\n"
                 << "public:\n"
                 << "    void OnCreate() override {\n"
                 << "    }\n\n"
                 << "    void OnUpdate(float dt) override {\n"
                 << "    }\n\n"
                 << "    void OnDestroy() override {\n"
                 << "    }\n"
                 << "};\n\n"
                 << "REGISTER_SCRIPT(" << name << ")\n";
            fout.close();
        } else if (ext == ".shader") {
            // Create a default ShaderLab template
            std::ofstream fout(absPath);
            fout << "Shader \"Custom/" << name << "\" {\n"
                 << "    Properties {\n"
                 << "        _MainTex (\"Main Texture\", 2D) = \"white\" {}\n"
                 << "        _Color (\"Color\", Color) = (1, 1, 1, 1)\n"
                 << "    }\n\n"
                 << "    SubShader {\n"
                 << "        Tags { \"RenderType\"=\"Opaque\" \"Queue\"=\"Geometry\" }\n\n"
                 << "        Pass {\n"
                 << "            Name \"Default\"\n\n"
                 << "            Cull Back\n"
                 << "            ZWrite On\n"
                 << "            ZTest LEqual\n\n"
                 << "            GLSLPROGRAM\n"
                 << "            #pragma vertex vert\n"
                 << "            #pragma fragment frag\n\n"
                 << "#version 460 core\n\n"
                 << "#ifdef VERTEX\n"
                 << "layout(location = 0) in vec3 a_Position;\n"
                 << "layout(location = 1) in vec3 a_Color;\n"
                 << "layout(location = 2) in vec2 a_TexCoord;\n\n"
                 << "uniform mat4 u_MVP;\n\n"
                 << "out vec3 v_Color;\n"
                 << "out vec2 v_TexCoord;\n\n"
                 << "void main() {\n"
                 << "    v_Color    = a_Color;\n"
                 << "    v_TexCoord = a_TexCoord;\n"
                 << "    gl_Position = u_MVP * vec4(a_Position, 1.0);\n"
                 << "}\n"
                 << "#endif\n\n"
                 << "#ifdef FRAGMENT\n"
                 << "in vec3 v_Color;\n"
                 << "in vec2 v_TexCoord;\n\n"
                 << "uniform sampler2D u_Texture;\n"
                 << "uniform int u_UseTexture;\n\n"
                 << "out vec4 FragColor;\n\n"
                 << "void main() {\n"
                 << "    vec3 baseColor = v_Color;\n"
                 << "    if (u_UseTexture == 1)\n"
                 << "        baseColor = texture(u_Texture, v_TexCoord).rgb;\n"
                 << "    FragColor = vec4(baseColor, 1.0);\n"
                 << "}\n"
                 << "#endif\n\n"
                 << "            ENDGLSL\n"
                 << "        }\n"
                 << "    }\n\n"
                 << "    FallBack \"VibeEngine/Unlit\"\n"
                 << "}\n";
            fout.close();
        }

        // Auto-compile and register new .shader files
        if (ext == ".shader") {
            auto shader = VE::Shader::CreateFromFile(absPath);
            if (shader) {
                VE::ShaderLibrary::Register(name, shader);
                VE_INFO("ShaderLibrary: Registered '{0}'", name);
            }
        }

        m_AssetDatabase.Refresh();
        VE_INFO("Created asset: {0}", relPath);
    }

    /// Scan Assets/ for .shader and .vmat files, compile+register any not yet in libraries.
    void ScanAndRegisterAssets() {
        std::string assetsRoot = m_AssetDatabase.GetAssetsRoot();
        if (!std::filesystem::exists(assetsRoot)) return;

        for (auto& entry : std::filesystem::recursive_directory_iterator(assetsRoot)) {
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().string();
            std::string name = entry.path().stem().string();

            if (ext == ".shader") {
                if (VE::ShaderLibrary::Exists(name)) continue;
                auto shader = VE::Shader::CreateFromFile(entry.path().generic_string());
                if (shader) {
                    VE::ShaderLibrary::Register(name, shader);
                    VE_INFO("ShaderLibrary: Registered '{0}' from Assets", name);
                }
            } else if (ext == ".vmat") {
                if (VE::MaterialLibrary::Get(name)) continue;
                auto mat = VE::Material::Load(entry.path().generic_string());
                if (mat) {
                    VE::MaterialLibrary::Register(mat);
                    VE_INFO("MaterialLibrary: Registered '{0}' from Assets", name);
                }
            }
        }
    }

    void MarkDirty() {
        m_SceneDirty = true;
        UpdateWindowTitle();
    }

    void ClearDirty() {
        m_SceneDirty = false;
        UpdateWindowTitle();
    }

    void UpdateWindowTitle() {
        std::string sceneName = GetCurrentSceneDisplayName();
        std::string title = std::string("VibeEngine - ") + sceneName;
        if (m_SceneDirty) title += " *";
        glfwSetWindowTitle(GetWindow().GetNativeWindow(), title.c_str());
    }

    std::string GetCurrentSceneDisplayName() const {
        if (m_CurrentScenePath.empty())
            return "Untitled";

        std::filesystem::path scenePath(m_CurrentScenePath);
        std::string sceneName = scenePath.stem().string();
        if (sceneName.empty())
            sceneName = scenePath.filename().string();
        return sceneName.empty() ? "Untitled" : sceneName;
    }

    void OpenInExternalEditor(const std::string& path) {
#ifdef _WIN32
        std::string absPath = std::filesystem::absolute(path).string();
        ShellExecuteA(nullptr, "open", absPath.c_str(), nullptr, nullptr, SW_SHOW);
        VE_INFO("Opening in external editor: {0}", absPath);
#endif
    }

    void NewScene() {
        m_Scene = std::make_shared<VE::Scene>();
        ClearSelection();
        m_CurrentScenePath.clear();
        m_CommandHistory.Clear();

        // Sync SceneManager
        m_SceneManager.UnloadAllScenes();
        m_SceneManager.AddScene(m_Scene, "Untitled");

        ClearDirty();
        VE_INFO("New scene created");
    }

    static bool PathSegmentEquals(const std::filesystem::path& segment, const char* text) {
        std::string value = segment.string();
        std::string expected = text;
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(expected.begin(), expected.end(), expected.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value == expected;
    }

    std::string GetSourceSceneMirrorPath(const std::string& scenePath) const {
        if (scenePath.empty())
            return {};

        std::error_code ec;
        std::filesystem::path sourceAssets = std::filesystem::path(VE_PROJECT_ROOT) / "Assets";
        sourceAssets = std::filesystem::weakly_canonical(sourceAssets, ec);
        if (ec)
            sourceAssets = std::filesystem::absolute(std::filesystem::path(VE_PROJECT_ROOT) / "Assets").lexically_normal();

        std::filesystem::path absoluteScene = std::filesystem::absolute(scenePath).lexically_normal();
        if (absoluteScene.extension() != ".vscene")
            return {};

        ec.clear();
        std::filesystem::path canonicalScene = std::filesystem::weakly_canonical(absoluteScene, ec);
        if (!ec)
            absoluteScene = canonicalScene;

        auto relToSource = absoluteScene.lexically_relative(sourceAssets);
        if (!relToSource.empty() && *relToSource.begin() != "..")
            return {};

        std::filesystem::path relAssetPath;
        bool foundAssets = false;
        for (auto it = absoluteScene.begin(); it != absoluteScene.end(); ++it) {
            if (!foundAssets) {
                foundAssets = PathSegmentEquals(*it, "Assets");
                continue;
            }
            relAssetPath /= *it;
        }

        if (!foundAssets || relAssetPath.empty())
            return {};

        std::filesystem::path mirrorPath = (sourceAssets / relAssetPath).lexically_normal();
        if (mirrorPath == absoluteScene)
            return {};

        return mirrorPath.generic_string();
    }

    void SerializeSceneWithSourceMirror(const std::string& scenePath) {
        VE::SceneSerializer serializer(m_Scene);
        serializer.Serialize(scenePath);

        const std::string mirrorPath = GetSourceSceneMirrorPath(scenePath);
        if (!mirrorPath.empty()) {
            std::filesystem::create_directories(std::filesystem::path(mirrorPath).parent_path());
            serializer.Serialize(mirrorPath);
            VE_INFO("Mirrored scene save to source Assets: {0}", mirrorPath);
        }
    }

    void SaveScene() {
        if (m_CurrentScenePath.empty()) { SaveSceneAs(); return; }
        SerializeSceneWithSourceMirror(m_CurrentScenePath);
        ClearDirty();
        m_AutoSaveTimer = 0.0f;
        // Remove auto-save file since scene is now saved
        std::filesystem::remove("ProjectSettings/AutoSave.vscene");
    }

    void SaveSceneAs() {
        std::string path = VE::FileDialog::SaveFile(s_SceneFilter, GetWindow().GetNativeWindow());
        if (!path.empty()) {
            m_CurrentScenePath = path;
            SerializeSceneWithSourceMirror(m_CurrentScenePath);
            ClearDirty();
            m_AutoSaveTimer = 0.0f;
            std::filesystem::remove("ProjectSettings/AutoSave.vscene");
        }
    }

    bool LoadSceneFromPath(const std::string& path) {
        if (path.empty())
            return false;

        auto nextScene = std::make_shared<VE::Scene>();
        VE::SceneSerializer serializer(nextScene);
        if (!serializer.Deserialize(path))
            return false;

        m_Scene = nextScene;
        ClearSelection();
        m_CurrentScenePath = path;

        auto rpView = m_Scene->GetAllEntitiesWith<VE::TransformComponent, VE::ReflectionProbeComponent>();
        for (auto e : rpView) {
            auto& rp = rpView.get<VE::ReflectionProbeComponent>(e);
            if (rp.BakeOnLoad)
                m_Scene->BakeReflectionProbe(e);
        }

        m_CommandHistory.Clear();
        m_SceneManager.UnloadAllScenes();
        m_SceneManager.AddScene(m_Scene, std::filesystem::path(path).stem().string(), path);
        ClearDirty();
        return true;
    }

    void LoadScene() {
        std::string path = VE::FileDialog::OpenFile(s_SceneFilter, GetWindow().GetNativeWindow());
        if (!path.empty())
            LoadSceneFromPath(path);
    }

    void LoadSceneAdditive() {
        std::string path = VE::FileDialog::OpenFile(s_SceneFilter, GetWindow().GetNativeWindow());
        if (!path.empty()) {
            int idx = m_SceneManager.LoadScene(path, VE::SceneLoadMode::Additive);
            if (idx >= 0) {
                VE_INFO("Additively loaded scene '{0}' (index {1})",
                    m_SceneManager.GetSceneInfo(idx).Name, idx);
            }
        }
    }

    // ── Scene Manager Panel ──────────────────────────────────────────

    void DrawSceneManagerPanel() {
        if (!m_ShowSceneManager) return;
        ImGui::Begin("Scene Manager", &m_ShowSceneManager);

        int sceneCount = m_SceneManager.GetSceneCount();
        ImGui::Text("Loaded Scenes: %d", sceneCount);
        ImGui::Separator();

        int activeIdx = m_SceneManager.GetActiveSceneIndex();
        int removeIdx = -1;

        for (int i = 0; i < sceneCount; i++) {
            auto& info = m_SceneManager.GetSceneInfo(i);
            ImGui::PushID(i);

            // Active indicator
            bool isActive = (i == activeIdx);
            if (isActive) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 1.0f, 0.2f, 1.0f));
                ImGui::Bullet();
                ImGui::PopStyleColor();
                ImGui::SameLine();
            }

            // Scene name
            ImGui::Text("%s", info.Name.c_str());
            ImGui::SameLine();

            // File path hint
            if (!info.FilePath.empty()) {
                ImGui::TextDisabled("(%s)", info.FilePath.c_str());
                ImGui::SameLine();
            }

            // Set Active button
            if (!isActive) {
                if (ImGui::SmallButton("Set Active")) {
                    m_SceneManager.SetActiveScene(i);
                    m_Scene = m_SceneManager.GetActiveScene();
                    m_SelectedEntity = {};
                    m_CommandHistory.Clear();
                }
                ImGui::SameLine();
            }

            // Physics toggle
            ImGui::Checkbox("Physics", &info.PhysicsEnabled);
            ImGui::SameLine();

            // Unload button (disabled if only one scene)
            if (sceneCount > 1) {
                if (ImGui::SmallButton("Unload"))
                    removeIdx = i;
            }

            ImGui::PopID();
        }

        // Process deferred removal
        if (removeIdx >= 0) {
            m_SceneManager.UnloadScene(removeIdx);
            // If active scene changed, update m_Scene
            m_Scene = m_SceneManager.GetActiveScene();
            if (!m_Scene) {
                m_Scene = std::make_shared<VE::Scene>();
                m_SceneManager.AddScene(m_Scene, "Untitled");
            }
            m_SelectedEntity = {};
            m_CommandHistory.Clear();
        }

        ImGui::Separator();

        // Transition controls
        ImGui::Text("Scene Transitions");
        static char transPath[256] = "";
        ImGui::InputText("Target Scene", transPath, sizeof(transPath));
        ImGui::SameLine();
        if (ImGui::SmallButton("Browse##TransTarget")) {
            std::string path = VE::FileDialog::OpenFile(s_SceneFilter, GetWindow().GetNativeWindow());
            if (!path.empty())
                strncpy(transPath, path.c_str(), sizeof(transPath) - 1);
        }

        static int transType = 1; // Fade
        ImGui::Combo("Transition", &transType, "None\0Fade\0CrossFade\0");
        static float transDuration = 1.0f;
        ImGui::SliderFloat("Duration (s)", &transDuration, 0.1f, 5.0f);

        if (ImGui::Button("Transition")) {
            if (strlen(transPath) > 0) {
                m_SceneManager.TransitionToScene(transPath,
                    static_cast<VE::TransitionType>(transType), transDuration);
            }
        }

        if (m_SceneManager.IsTransitioning()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Transitioning...");
        }

        ImGui::End();
    }

    // Draw a fullscreen black overlay during scene transitions
    void DrawTransitionOverlay() {
        if (!m_SceneManager.IsTransitioning()) return;

        m_SceneManager.UpdateTransition(m_DeltaTime);
        float alpha = m_SceneManager.GetTransitionOverlayAlpha();
        if (alpha <= 0.0f) return;

        // After transition load phase, sync m_Scene to the new active scene
        if (m_SceneManager.GetTransitionPhase() == VE::TransitionPhase::FadeIn) {
            auto newActive = m_SceneManager.GetActiveScene();
            if (newActive && newActive != m_Scene) {
                m_Scene = newActive;
                m_CurrentScenePath = m_SceneManager.GetSceneInfo(
                    m_SceneManager.GetActiveSceneIndex()).FilePath;
                m_SelectedEntity = {};
                m_CommandHistory.Clear();
            }
        }

        // Draw fullscreen black overlay via ImGui
        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        ImGui::SetNextWindowBgAlpha(alpha);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBringToFrontOnFocus
            | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
        ImGui::Begin("##TransitionOverlay", nullptr, flags);
        ImGui::End();
        ImGui::PopStyleColor();
    }

    // ── Build Panel ──────────────────────────────────────────────────

    void DrawBuildPanel() {
        if (!m_ShowBuildPanel) return;
        ImGui::Begin("Build Settings", &m_ShowBuildPanel);

        // Game name
        char nameBuf[256];
        strncpy(nameBuf, m_BuildSettings.GameName.c_str(), sizeof(nameBuf) - 1);
        nameBuf[sizeof(nameBuf) - 1] = 0;
        if (ImGui::InputText("Game Name", nameBuf, sizeof(nameBuf)))
            m_BuildSettings.GameName = nameBuf;

        // Output directory
        char outBuf[512];
        strncpy(outBuf, m_BuildSettings.OutputDir.c_str(), sizeof(outBuf) - 1);
        outBuf[sizeof(outBuf) - 1] = 0;
        if (ImGui::InputText("Output Directory", outBuf, sizeof(outBuf)))
            m_BuildSettings.OutputDir = outBuf;

        // Startup scene
        ImGui::Text("Startup Scene:");
        ImGui::SameLine();
        if (m_BuildSettings.StartupScene.empty()) {
            ImGui::TextDisabled("(current scene)");
        } else {
            ImGui::Text("%s", m_BuildSettings.StartupScene.c_str());
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Use Current")) {
            m_BuildSettings.StartupScene = m_CurrentScenePath;
        }

        // Config
        ImGui::Checkbox("Release Mode", &m_BuildSettings.ReleaseMode);

        ImGui::Separator();

        // Build button
        if (m_BuildInProgress) {
            ImGui::TextColored({1, 1, 0, 1}, "Building...");
        } else {
            if (ImGui::Button("Build", ImVec2(120, 30))) {
                m_BuildLog.clear();
                m_BuildInProgress = true;

                // Use current scene if no startup scene set
                if (m_BuildSettings.StartupScene.empty() && !m_CurrentScenePath.empty())
                    m_BuildSettings.StartupScene = m_CurrentScenePath;

                std::string cmakePath =
                    "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\Common7\\IDE"
                    "\\CommonExtensions\\Microsoft\\CMake\\CMake\\bin\\cmake.exe";

                bool ok = VE::BuildExporter::Build(
                    m_BuildSettings,
                    std::string(VE_PROJECT_ROOT),
                    cmakePath,
                    m_BuildLog);

                m_BuildInProgress = false;

                if (ok)
                    VE_INFO("Build succeeded!");
                else
                    VE_ERROR("Build failed!");
            }
            ImGui::SameLine();
            if (ImGui::Button("Open Output Folder", ImVec2(160, 30))) {
                std::string fullPath = std::string(VE_PROJECT_ROOT) + "/" + m_BuildSettings.OutputDir;
                std::filesystem::create_directories(fullPath);
                std::string cmd = "explorer \"" + std::filesystem::absolute(fullPath).string() + "\"";
                std::system(cmd.c_str());
            }
        }

        // Build log
        if (!m_BuildLog.empty()) {
            ImGui::Separator();
            ImGui::Text("Build Log:");
            ImGui::BeginChild("BuildLog", ImVec2(0, 200), true);
            ImGui::TextUnformatted(m_BuildLog.c_str());
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
        }

        ImGui::End();
    }

    // ── LOD Test Scene ────────────────────────────────────────────────

    void CreateLODTestScene() {
        NewScene();

        // Generate LOD sphere meshes with varying detail
        auto sphereHigh = VE::LODMeshGenerator::CreateSphere(32, 64);   // 4096 tris
        auto sphereMed  = VE::LODMeshGenerator::CreateSphere(16, 32);   // 1024 tris
        auto sphereLow  = VE::LODMeshGenerator::CreateSphere(8, 16);    // 256 tris
        auto sphereMin  = VE::LODMeshGenerator::CreateSphere(4, 8);     // 64 tris

        auto litMat = VE::MaterialLibrary::Get("Lit");

        // Create a grid of LOD spheres
        float spacing = 4.0f;
        int gridSize = 5;
        for (int x = 0; x < gridSize; ++x) {
            for (int z = 0; z < gridSize; ++z) {
                float px = (x - gridSize / 2) * spacing;
                float pz = (z - gridSize / 2) * spacing;

                std::string name = "LOD Sphere (" + std::to_string(x) + "," + std::to_string(z) + ")";
                auto e = m_Scene->CreateEntity(name);

                auto& tc = e.GetComponent<VE::TransformComponent>();
                tc.Position = { px, 1.0f, pz };

                auto& mr = e.AddComponent<VE::MeshRendererComponent>();
                mr.Mesh = sphereHigh->VAO;
                mr.Mat = litMat;
                // Vary colors across the grid
                float hue = static_cast<float>(x * gridSize + z) / (gridSize * gridSize);
                mr.Color = { 0.3f + hue * 0.7f, 0.5f + (1.0f - hue) * 0.5f, 0.3f + hue * 0.3f, 1.0f };
                mr.LocalBounds = sphereHigh->BoundingBox;

                // Add LOD group with 4 levels
                auto& lod = e.AddComponent<VE::LODGroupComponent>();
                lod.Levels.push_back({ sphereHigh->VAO, "", -1, 0, 10.0f });   // LOD 0: < 10m
                lod.Levels.push_back({ sphereMed->VAO,  "", -1, 0, 25.0f });   // LOD 1: 10-25m
                lod.Levels.push_back({ sphereLow->VAO,  "", -1, 0, 50.0f });   // LOD 2: 25-50m
                lod.Levels.push_back({ sphereMin->VAO,  "", -1, 0, 100.0f });  // LOD 3: 50-100m
                lod.CullDistance = 150.0f;
            }
        }

        // Ground plane
        {
            auto e = m_Scene->CreateEntity("Ground");
            auto& tc = e.GetComponent<VE::TransformComponent>();
            tc.Position = { 0, -0.5f, 0 };
            tc.Scale = { 40, 1, 40 };
            auto& mr = e.AddComponent<VE::MeshRendererComponent>();
            mr.Mesh = VE::MeshLibrary::GetCube();
            mr.Mat = litMat;
            mr.Color = { 0.5f, 0.5f, 0.5f, 1.0f };
        }

        // Directional light
        {
            auto e = m_Scene->CreateEntity("Directional Light");
            auto& tc = e.GetComponent<VE::TransformComponent>();
            tc.Position = { 0, 10, 0 };
            tc.Rotation = EulerFromForwardDirection(glm::vec3(0.3f, 1.0f, 0.5f));
            e.AddComponent<VE::DirectionalLightComponent>();
        }

        // Main camera
        {
            auto e = m_Scene->CreateEntity("Main Camera");
            auto& tag = e.GetComponent<VE::TagComponent>();
            tag.GameObjectTag = "MainCamera";
            auto& tc = e.GetComponent<VE::TransformComponent>();
            tc.Position = { 0, 5, 15 };
            tc.Rotation = { -15, 0, 0 };
            auto& cam = e.AddComponent<VE::CameraComponent>();
            cam.FOV = 60.0f;
        }

        // Store the generated meshes so they stay alive
        m_LODMeshes = { sphereHigh, sphereMed, sphereLow, sphereMin };

        VE_INFO("Created LOD Test Scene: {} spheres with 4 LOD levels", gridSize * gridSize);
    }

    // ── Editor settings persistence ──────────────────────────────────

    // ── Built-in player controller for input testing ──────────────────
    void SetupDefaultInputActions() {
        // Try loading saved input map first
        auto& playerMap = VE::InputActions::CreateMap("Player");
        if (playerMap.LoadFromFile("ProjectSettings/InputActions.yaml"))
            return; // Loaded custom bindings

        // ── Default Player action map ──
        // Movement (WASD + Gamepad left stick)
        {
            auto& moveH = playerMap.AddAction("MoveHorizontal", VE::ActionType::Axis);
            moveH.AddBinding({ VE::BindingSource::Key, static_cast<int>(VE::KeyCode::D), 0,  1.0f });
            moveH.AddBinding({ VE::BindingSource::Key, static_cast<int>(VE::KeyCode::A), 0, -1.0f });
            moveH.AddBinding({ VE::BindingSource::Key, static_cast<int>(VE::KeyCode::Right), 0,  1.0f });
            moveH.AddBinding({ VE::BindingSource::Key, static_cast<int>(VE::KeyCode::Left), 0, -1.0f });
            moveH.AddBinding({ VE::BindingSource::GamepadAxis, static_cast<int>(VE::GamepadAxis::LeftX), 0, 1.0f });
        }
        {
            auto& moveV = playerMap.AddAction("MoveVertical", VE::ActionType::Axis);
            moveV.AddBinding({ VE::BindingSource::Key, static_cast<int>(VE::KeyCode::W), 0,  1.0f });
            moveV.AddBinding({ VE::BindingSource::Key, static_cast<int>(VE::KeyCode::S), 0, -1.0f });
            moveV.AddBinding({ VE::BindingSource::Key, static_cast<int>(VE::KeyCode::Up), 0,  1.0f });
            moveV.AddBinding({ VE::BindingSource::Key, static_cast<int>(VE::KeyCode::Down), 0, -1.0f });
            moveV.AddBinding({ VE::BindingSource::GamepadAxis, static_cast<int>(VE::GamepadAxis::LeftY), 0, -1.0f }); // Y inverted
        }
        // Look (mouse + right stick)
        {
            auto& lookH = playerMap.AddAction("LookHorizontal", VE::ActionType::Axis);
            lookH.AddBinding({ VE::BindingSource::MouseAxisX, 0, 0, 0.1f });
            lookH.AddBinding({ VE::BindingSource::GamepadAxis, static_cast<int>(VE::GamepadAxis::RightX), 0, 1.0f });
        }
        {
            auto& lookV = playerMap.AddAction("LookVertical", VE::ActionType::Axis);
            lookV.AddBinding({ VE::BindingSource::MouseAxisY, 0, 0, 0.1f });
            lookV.AddBinding({ VE::BindingSource::GamepadAxis, static_cast<int>(VE::GamepadAxis::RightY), 0, 1.0f });
        }
        // Actions
        {
            auto& jump = playerMap.AddAction("Jump", VE::ActionType::Button);
            jump.AddBinding({ VE::BindingSource::Key, static_cast<int>(VE::KeyCode::Space), 0, 1.0f });
            jump.AddBinding({ VE::BindingSource::GamepadButton, static_cast<int>(VE::GamepadButton::A), 0, 1.0f });
        }
        {
            auto& fire = playerMap.AddAction("Fire", VE::ActionType::Button);
            fire.AddBinding({ VE::BindingSource::MouseButton, static_cast<int>(VE::MouseButton::Left), 0, 1.0f });
            fire.AddBinding({ VE::BindingSource::GamepadButton, static_cast<int>(VE::GamepadButton::RightBumper), 0, 1.0f });
            fire.AddBinding({ VE::BindingSource::GamepadAxis, static_cast<int>(VE::GamepadAxis::RightTrigger), 0, 1.0f });
        }
        {
            auto& sprint = playerMap.AddAction("Sprint", VE::ActionType::Button);
            sprint.AddBinding({ VE::BindingSource::Key, static_cast<int>(VE::KeyCode::LeftShift), 0, 1.0f });
            sprint.AddBinding({ VE::BindingSource::GamepadButton, static_cast<int>(VE::GamepadButton::LeftThumb), 0, 1.0f });
        }

        // Save defaults
        std::filesystem::create_directories("ProjectSettings");
        playerMap.SaveToFile("ProjectSettings/InputActions.yaml");
    }

    void SaveEditorSettings() {
        YAML::Emitter out;
        out << YAML::BeginMap;

        // Last opened scene
        out << YAML::Key << "LastScene" << YAML::Value << m_CurrentScenePath;

        // Main window state
        if (auto* window = GetWindow().GetNativeWindow()) {
            int width = 0;
            int height = 0;
            int posX = 0;
            int posY = 0;
            glfwGetWindowSize(window, &width, &height);
            glfwGetWindowPos(window, &posX, &posY);

            out << YAML::Key << "MainWindow" << YAML::Value << YAML::BeginMap;
            out << YAML::Key << "Width" << YAML::Value << width;
            out << YAML::Key << "Height" << YAML::Value << height;
            out << YAML::Key << "PosX" << YAML::Value << posX;
            out << YAML::Key << "PosY" << YAML::Value << posY;
            out << YAML::Key << "Maximized" << YAML::Value
                << (glfwGetWindowAttrib(window, GLFW_MAXIMIZED) == GLFW_TRUE);
            out << YAML::EndMap;
        }

        // Camera state
        out << YAML::Key << "Camera" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Mode" << YAML::Value
            << (m_Camera.GetMode() == VE::CameraMode::Perspective3D ? "Perspective3D" : "Orthographic2D");

        // 2D state
        out << YAML::Key << "Position2D" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << m_Camera.GetPosition().x << m_Camera.GetPosition().y << YAML::EndSeq;
        out << YAML::Key << "Zoom" << YAML::Value << m_Camera.GetZoom();

        // 3D state
        auto fp = m_Camera.GetFocalPoint();
        out << YAML::Key << "FocalPoint" << YAML::Value << YAML::Flow
            << YAML::BeginSeq << fp.x << fp.y << fp.z << YAML::EndSeq;
        out << YAML::Key << "Distance" << YAML::Value << m_Camera.GetDistance();
        out << YAML::Key << "Yaw" << YAML::Value << m_Camera.GetYaw();
        out << YAML::Key << "Pitch" << YAML::Value << m_Camera.GetPitch();

        out << YAML::EndMap; // Camera

        // Window visibility
        out << YAML::Key << "ShowGameView" << YAML::Value << m_ShowGameView;
        out << YAML::Key << "ShowRenderPipeline" << YAML::Value << m_ShowPipelineSettings;
        out << YAML::Key << "ShowScripting" << YAML::Value << m_ShowScripting;
        out << YAML::Key << "ShowContentBrowser" << YAML::Value << m_ShowContentBrowser;
        out << YAML::Key << "ShowHierarchy" << YAML::Value << m_ShowHierarchy;
        out << YAML::Key << "ShowMaterialEditor" << YAML::Value << m_ShowMaterialEditor;

        out << YAML::Key << "ShowProfiler" << YAML::Value << m_ShowProfiler;
        out << YAML::Key << "ShowColliders" << YAML::Value << m_ShowColliders;

        out << YAML::Key << "ShowSceneManager" << YAML::Value << m_ShowSceneManager;

        // Auto-save settings
        out << YAML::Key << "AutoSaveEnabled" << YAML::Value << m_AutoSaveEnabled;
        out << YAML::Key << "AutoSaveInterval" << YAML::Value << m_AutoSaveInterval;

        out << YAML::EndMap;

        std::filesystem::create_directories("ProjectSettings");
        std::ofstream fout("ProjectSettings/EditorSettings.yaml");
        if (fout) {
            fout << out.c_str();
            VE_INFO("Editor settings saved");
        }
    }

    void LoadEditorSettings() {
        const std::string path = "ProjectSettings/EditorSettings.yaml";
        if (!std::filesystem::exists(path)) return;

        try {
            YAML::Node root = YAML::LoadFile(path);

            // Restore main window size and placement after GLFW has created it.
            if (auto winNode = root["MainWindow"]) {
                if (auto* window = GetWindow().GetNativeWindow()) {
                    int width = winNode["Width"].as<int>(1280);
                    int height = winNode["Height"].as<int>(720);
                    width = std::clamp(width, 640, 8192);
                    height = std::clamp(height, 480, 8192);

                    if (winNode["PosX"] && winNode["PosY"])
                        glfwSetWindowPos(window, winNode["PosX"].as<int>(), winNode["PosY"].as<int>());

                    glfwSetWindowSize(window, width, height);
                    if (winNode["Maximized"].as<bool>(false))
                        glfwMaximizeWindow(window);
                }
            }

            // Restore last scene
            if (root["LastScene"]) {
                std::string scenePath = root["LastScene"].as<std::string>("");
                if (!scenePath.empty() && std::filesystem::exists(scenePath)) {
                    m_Scene = std::make_shared<VE::Scene>();
                    VE::SceneSerializer serializer(m_Scene);
                    if (serializer.Deserialize(scenePath)) {
                        m_CurrentScenePath = scenePath;
                        // Sync SceneManager
                        m_SceneManager.UnloadAllScenes();
                        m_SceneManager.AddScene(m_Scene,
                            std::filesystem::path(scenePath).stem().string(), scenePath);
                    }
                }
            }

            // Restore camera
            if (root["Camera"]) {
                auto cam = root["Camera"];
                std::string mode = cam["Mode"].as<std::string>("Perspective3D");
                m_Camera.SetMode(mode == "Orthographic2D"
                    ? VE::CameraMode::Orthographic2D : VE::CameraMode::Perspective3D);

                // 2D state
                if (cam["Position2D"] && cam["Position2D"].size() == 2) {
                    glm::vec2 pos(cam["Position2D"][0].as<float>(0.0f),
                                  cam["Position2D"][1].as<float>(0.0f));
                    m_Camera.SetPosition2D(pos);
                }
                if (cam["Zoom"])
                    m_Camera.SetZoom(cam["Zoom"].as<float>(1.0f));

                // 3D state
                if (cam["FocalPoint"] && cam["FocalPoint"].size() == 3) {
                    glm::vec3 fp(cam["FocalPoint"][0].as<float>(0.0f),
                                 cam["FocalPoint"][1].as<float>(0.0f),
                                 cam["FocalPoint"][2].as<float>(0.0f));
                    m_Camera.SetFocalPoint(fp);
                }
                if (cam["Distance"])
                    m_Camera.SetDistance(cam["Distance"].as<float>(5.0f));
                if (cam["Yaw"])
                    m_Camera.SetYaw(cam["Yaw"].as<float>(-45.0f));
                if (cam["Pitch"])
                    m_Camera.SetPitch(cam["Pitch"].as<float>(30.0f));
            }

            // Window visibility
            if (root["ShowGameView"])
                m_ShowGameView = root["ShowGameView"].as<bool>(false);
            if (root["ShowRenderPipeline"])
                m_ShowPipelineSettings = root["ShowRenderPipeline"].as<bool>(false);
            if (root["ShowScripting"])
                m_ShowScripting = root["ShowScripting"].as<bool>(false);
            if (root["ShowContentBrowser"])
                m_ShowContentBrowser = root["ShowContentBrowser"].as<bool>(true);
            if (root["ShowHierarchy"])
                m_ShowHierarchy = root["ShowHierarchy"].as<bool>(true);
            if (root["ShowMaterialEditor"])
                m_ShowMaterialEditor = root["ShowMaterialEditor"].as<bool>(false);

            if (root["ShowProfiler"])
                m_ShowProfiler = root["ShowProfiler"].as<bool>(false);
            if (root["ShowColliders"])
                m_ShowColliders = root["ShowColliders"].as<bool>(true);

            if (root["ShowSceneManager"])
                m_ShowSceneManager = root["ShowSceneManager"].as<bool>(false);

            // Auto-save settings
            if (root["AutoSaveEnabled"])
                m_AutoSaveEnabled = root["AutoSaveEnabled"].as<bool>(true);
            if (root["AutoSaveInterval"])
                m_AutoSaveInterval = root["AutoSaveInterval"].as<float>(120.0f);

            // Check for crash recovery: auto-save file newer than last saved scene
            const std::string autoSavePath = "ProjectSettings/AutoSave.vscene";
            if (std::filesystem::exists(autoSavePath)) {
                bool shouldRecover = false;
                if (m_CurrentScenePath.empty() || !std::filesystem::exists(m_CurrentScenePath)) {
                    // No saved scene — any auto-save is worth recovering
                    shouldRecover = true;
                } else {
                    auto autoSaveTime = std::filesystem::last_write_time(autoSavePath);
                    auto sceneTime = std::filesystem::last_write_time(m_CurrentScenePath);
                    shouldRecover = (autoSaveTime > sceneTime);
                }
                if (shouldRecover) {
                    m_ShowRecoveryPopup = true;
                    VE_INFO("Auto-save recovery file detected");
                }
            }

            VE_INFO("Editor settings restored");
        } catch (const std::exception& e) {
            VE_WARN("Failed to load editor settings: {}", e.what());
        }
    }

    // ── GPU resource management ───────────────────────────────────────

    void ClearEntityGPUResources() {
        auto view = m_Scene->GetAllEntitiesWith<VE::MeshRendererComponent>();
        for (auto entityID : view) {
            auto& mr = view.get<VE::MeshRendererComponent>(entityID);
            for (int i = 0; i < VE::MeshLibrary::GetMeshCount(); i++) {
                if (VE::MeshLibrary::GetMeshByIndex(i) == mr.Mesh) {
                    m_EntityMeshIndex[static_cast<uint32_t>(entityID)] = i;
                    break;
                }
            }
            mr.Mesh.reset();
            mr.Mat.reset();
        }
        m_Scene->GetPipelineSettings().SkyTexture.reset();
    }

    void RestoreEntityGPUResources() {
        auto view = m_Scene->GetAllEntitiesWith<VE::MeshRendererComponent>();
        for (auto entityID : view) {
            auto& mr = view.get<VE::MeshRendererComponent>(entityID);
            auto it = m_EntityMeshIndex.find(static_cast<uint32_t>(entityID));
            if (it != m_EntityMeshIndex.end()) {
                mr.Mesh = VE::MeshLibrary::GetMeshByIndex(it->second);
                mr.Mat = VE::MeshLibrary::IsLitMesh(it->second)
                    ? VE::MaterialLibrary::Get("Lit")
                    : VE::MaterialLibrary::Get("Default");
            }
        }
        m_EntityMeshIndex.clear();
        auto& ps = m_Scene->GetPipelineSettings();
        if (!ps.SkyTexturePath.empty())
            ps.SkyTexture = VE::Texture2D::Create(ps.SkyTexturePath);
    }

    // ── Component Context Menu (Reset / Copy / Paste / Remove) ─────

    template<typename T>
    bool DrawComponentContextMenu(const char* popupID, const char* typeName, bool& outRemove) {
        // Returns true if component was reset (caller should re-read ref)
        bool wasReset = false;
        if (ImGui::BeginPopupContextItem(popupID)) {
            if (ImGui::MenuItem("Reset")) {
                auto before = m_CommandHistory.CaptureSnapshot();
                m_SelectedEntity.GetComponent<T>() = T{};
                m_CommandHistory.RecordPropertyEdit("Reset Component", std::move(before));
                wasReset = true;
            }
            if (ImGui::MenuItem("Copy Component")) {
                m_ClipboardComponentType = typeName;
                m_ClipboardComponentData = m_SelectedEntity.GetComponent<T>();
            }
            bool canPaste = (m_ClipboardComponentType == typeName);
            if (ImGui::MenuItem("Paste Component", nullptr, false, canPaste)) {
                auto before = m_CommandHistory.CaptureSnapshot();
                m_SelectedEntity.GetComponent<T>() = std::any_cast<T>(m_ClipboardComponentData);
                m_CommandHistory.RecordPropertyEdit("Paste Component", std::move(before));
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Remove Component"))
                outRemove = true;
            ImGui::EndPopup();
        }
        return wasReset;
    }

    // ── Multi-Select Helpers ────────────────────────────────────────

    void SelectEntity(VE::Entity entity) {
        m_SelectedEntity = entity;
        if (entity)
            m_SelectedEntities.insert(entity.GetHandle());
        m_SelectedAssetPath.clear();
    }

    void SelectEntityOnly(VE::Entity entity) {
        // Single select: clear others, select this one
        m_SelectedEntities.clear();
        SelectEntity(entity);
        if (entity)
            m_LastClickedEntity = entity.GetHandle();
    }

    void ToggleEntitySelection(entt::entity entityID) {
        if (m_SelectedEntities.count(entityID)) {
            m_SelectedEntities.erase(entityID);
            if (m_SelectedEntity.GetHandle() == entityID) {
                // Pick another primary, or clear
                if (!m_SelectedEntities.empty())
                    m_SelectedEntity = VE::Entity(*m_SelectedEntities.rbegin(), &*m_Scene);
                else
                    m_SelectedEntity = {};
            }
        } else {
            m_SelectedEntities.insert(entityID);
            m_SelectedEntity = VE::Entity(entityID, &*m_Scene);
        }
        m_LastClickedEntity = entityID;
        m_SelectedAssetPath.clear();
    }

    void ClearSelection() {
        m_SelectedEntity = {};
        m_SelectedEntities.clear();
    }

    bool IsEntitySelected(entt::entity entityID) const {
        return m_SelectedEntities.count(entityID) > 0;
    }

    // Build flat list of hierarchy entities in display order (for Shift+Click range select)
    void CollectHierarchyOrder(entt::entity entityID, std::vector<entt::entity>& out) {
        out.push_back(entityID);
        auto& reg = m_Scene->GetRegistry();
        if (reg.valid(entityID) && reg.any_of<VE::RelationshipComponent>(entityID)) {
            auto& rel = reg.get<VE::RelationshipComponent>(entityID);
            for (auto child : rel.Children)
                CollectHierarchyOrder(child, out);
        }
    }

    std::vector<entt::entity> GetHierarchyDisplayOrder() {
        std::vector<entt::entity> order;
        auto view = m_Scene->GetAllEntitiesWith<VE::RelationshipComponent>();
        std::vector<entt::entity> roots;
        for (auto entityID : view) {
            auto& rel = view.get<VE::RelationshipComponent>(entityID);
            if (rel.Parent == entt::null)
                roots.push_back(entityID);
        }
        std::reverse(roots.begin(), roots.end());
        for (auto root : roots)
            CollectHierarchyOrder(root, order);
        return order;
    }

    void ShiftSelectRange(entt::entity clickedEntity) {
        if (m_LastClickedEntity == entt::null) {
            // No previous click: just single-select
            SelectEntityOnly(VE::Entity(clickedEntity, &*m_Scene));
            return;
        }
        auto order = GetHierarchyDisplayOrder();
        int startIdx = -1, endIdx = -1;
        for (int i = 0; i < (int)order.size(); i++) {
            if (order[i] == m_LastClickedEntity) startIdx = i;
            if (order[i] == clickedEntity)       endIdx = i;
        }
        if (startIdx < 0 || endIdx < 0) {
            SelectEntityOnly(VE::Entity(clickedEntity, &*m_Scene));
            return;
        }
        if (startIdx > endIdx) std::swap(startIdx, endIdx);
        m_SelectedEntities.clear();
        for (int i = startIdx; i <= endIdx; i++)
            m_SelectedEntities.insert(order[i]);
        m_SelectedEntity = VE::Entity(clickedEntity, &*m_Scene);
        m_SelectedAssetPath.clear();
        // Note: don't update m_LastClickedEntity on shift-click (anchor stays)
    }

    // Duplicate a single entity, returns the new entity
    VE::Entity DuplicateSingleEntity(entt::entity srcEntity) {
        auto& reg = m_Scene->GetRegistry();
        auto& srcTag = reg.get<VE::TagComponent>(srcEntity);
        auto newEntity = m_Scene->CreateEntity(srcTag.Tag + " (Copy)");
        auto dst = newEntity.GetHandle();

        if (reg.any_of<VE::TransformComponent>(srcEntity))
            reg.get_or_emplace<VE::TransformComponent>(dst) = reg.get<VE::TransformComponent>(srcEntity);
        if (reg.any_of<VE::MeshRendererComponent>(srcEntity))
            reg.get_or_emplace<VE::MeshRendererComponent>(dst) = reg.get<VE::MeshRendererComponent>(srcEntity);
        if (reg.any_of<VE::HPWaterComponent>(srcEntity)) {
            auto copied = reg.get<VE::HPWaterComponent>(srcEntity);
            copied._Mesh.reset();
            copied._VertexBuffer.reset();
            copied._IndexBuffer.reset();
            copied._Current.clear();
            copied._Previous.clear();
            copied._Next.clear();
            copied._Vertices.clear();
            copied._Indices.clear();
            copied._Accumulator = 0.0f;
            copied._ImpulseTimer = 0.0f;
            copied._NeedsRebuild = true;
            reg.get_or_emplace<VE::HPWaterComponent>(dst) = std::move(copied);
        }
        if (reg.any_of<VE::DirectionalLightComponent>(srcEntity))
            reg.get_or_emplace<VE::DirectionalLightComponent>(dst) = reg.get<VE::DirectionalLightComponent>(srcEntity);
        if (reg.any_of<VE::AreaLightComponent>(srcEntity))
            reg.get_or_emplace<VE::AreaLightComponent>(dst) = reg.get<VE::AreaLightComponent>(srcEntity);

            // Copy ReflectionProbeComponent (reset runtime probe)
            if (reg.any_of<VE::ReflectionProbeComponent>(srcEntity)) {
                auto& src = reg.get<VE::ReflectionProbeComponent>(srcEntity);
                auto& dstRP = reg.get_or_emplace<VE::ReflectionProbeComponent>(dst);
                dstRP.Resolution = src.Resolution;
                dstRP.BoxSize = src.BoxSize;
                dstRP.BakeOnLoad = src.BakeOnLoad;
                dstRP._Probe = nullptr;
                dstRP._IsBaked = false;
            }

            // Copy LightProbeComponent
            if (reg.any_of<VE::LightProbeComponent>(srcEntity))
                reg.get_or_emplace<VE::LightProbeComponent>(dst) = reg.get<VE::LightProbeComponent>(srcEntity);

            // Copy LightmapComponent
            if (reg.any_of<VE::LightmapComponent>(srcEntity))
                reg.get_or_emplace<VE::LightmapComponent>(dst) = reg.get<VE::LightmapComponent>(srcEntity);

        if (reg.any_of<VE::RigidbodyComponent>(srcEntity)) {
            auto rb = reg.get<VE::RigidbodyComponent>(srcEntity);
            rb._JoltBodyID = 0xFFFFFFFF;
            reg.get_or_emplace<VE::RigidbodyComponent>(dst) = rb;
        }
        if (reg.any_of<VE::BoxColliderComponent>(srcEntity))
            reg.get_or_emplace<VE::BoxColliderComponent>(dst) = reg.get<VE::BoxColliderComponent>(srcEntity);
        if (reg.any_of<VE::SphereColliderComponent>(srcEntity))
            reg.get_or_emplace<VE::SphereColliderComponent>(dst) = reg.get<VE::SphereColliderComponent>(srcEntity);
        if (reg.any_of<VE::CapsuleColliderComponent>(srcEntity))
            reg.get_or_emplace<VE::CapsuleColliderComponent>(dst) = reg.get<VE::CapsuleColliderComponent>(srcEntity);
        if (reg.any_of<VE::MeshColliderComponent>(srcEntity))
            reg.get_or_emplace<VE::MeshColliderComponent>(dst) = reg.get<VE::MeshColliderComponent>(srcEntity);
        if (reg.any_of<VE::CameraComponent>(srcEntity))
            reg.get_or_emplace<VE::CameraComponent>(dst) = reg.get<VE::CameraComponent>(srcEntity);
        if (reg.any_of<VE::ScriptComponent>(srcEntity)) {
            auto sc = reg.get<VE::ScriptComponent>(srcEntity);
            sc._Instance = nullptr;
            reg.get_or_emplace<VE::ScriptComponent>(dst) = sc;
        }
        return newEntity;
    }

    // ── Duplicate Entity ─────────────────────────────────────────────

    void DuplicateSelectedEntity() {
        if (m_SelectedEntity.GetHandle() == entt::null && m_SelectedEntities.empty()) return;

        if (m_SelectedEntities.size() > 1) {
            // Batch duplicate
            m_CommandHistory.Execute("Duplicate Entities", [this]() {
                std::set<entt::entity> newSelection;
                VE::Entity lastNew;
                for (auto srcEntity : m_SelectedEntities) {
                    if (m_Scene->GetRegistry().valid(srcEntity)) {
                        auto newEntity = DuplicateSingleEntity(srcEntity);
                        newSelection.insert(newEntity.GetHandle());
                        lastNew = newEntity;
                    }
                }
                m_SelectedEntities = newSelection;
                m_SelectedEntity = lastNew;
            });
            return;
        }

        m_CommandHistory.Execute("Duplicate Entity", [this]() {
            auto newEntity = DuplicateSingleEntity(m_SelectedEntity.GetHandle());
            SelectEntityOnly(newEntity);
        });
    }

    // Batch delete all selected entities
    void DeleteSelectedEntities() {
        if (m_SelectedEntities.empty() && !m_SelectedEntity) return;

        if (m_SelectedEntities.size() > 1) {
            m_CommandHistory.Execute("Delete Entities", [this]() {
                for (auto entityID : m_SelectedEntities) {
                    if (m_Scene->GetRegistry().valid(entityID))
                        m_Scene->DestroyEntity(VE::Entity(entityID, &*m_Scene));
                }
                ClearSelection();
            });
        } else {
            m_CommandHistory.Execute("Delete Entity", [this]() {
                m_Scene->DestroyEntity(m_SelectedEntity);
                ClearSelection();
            });
        }
    }

    // ── Copy / Cut / Paste Entity ───────────────────────────────────────

    void CopySelectedEntity() {
        if (m_SelectedEntity.GetHandle() == entt::null) return;
        m_EntityClipboard = VE::SceneSerializer::SerializeEntityToString(m_SelectedEntity, *m_Scene);
    }

    void CutSelectedEntity() {
        if (m_SelectedEntity.GetHandle() == entt::null) return;
        CopySelectedEntity();
        m_CommandHistory.Execute("Cut Entity", [this]() {
            m_Scene->DestroyEntity(m_SelectedEntity);
            ClearSelection();
        });
    }

    void PasteEntity() {
        if (m_EntityClipboard.empty()) return;
        m_CommandHistory.Execute("Paste Entity", [this]() {
            VE::Entity pasted = VE::SceneSerializer::InstantiateFromString(m_EntityClipboard, *m_Scene);
            if (pasted.IsValid()) {
                if (pasted.HasComponent<VE::TransformComponent>()) {
                    auto& tc = pasted.GetComponent<VE::TransformComponent>();
                    tc.Position[0] += 1.0f;
                    tc.Position[1] += 1.0f;
                    tc.Position[2] += 1.0f;
                }
                SelectEntityOnly(pasted);
            }
        });
    }

    // Project entity world position to screen coordinates
    ImVec2 ProjectEntityToScreen(entt::entity entityID) {
        glm::mat4 worldMat = m_Scene->GetWorldTransform(entityID);
        glm::vec3 worldPos = glm::vec3(worldMat[3]);
        glm::vec4 clip = m_FrameVP * glm::vec4(worldPos, 1.0f);
        if (clip.w <= 0.0f) return { -1.0f, -1.0f };
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        float screenX = m_SceneVpX + (ndc.x * 0.5f + 0.5f) * m_SceneVpW;
        float screenY = m_SceneVpY + (1.0f - (ndc.y * 0.5f + 0.5f)) * m_SceneVpH;
        return { screenX, screenY };
    }

    // Box select: find all entities whose screen projection falls within the rectangle
    void PerformBoxSelect(ImVec2 boxMin, ImVec2 boxMax, bool additive) {
        float minX = std::min(boxMin.x, boxMax.x);
        float maxX = std::max(boxMin.x, boxMax.x);
        float minY = std::min(boxMin.y, boxMax.y);
        float maxY = std::max(boxMin.y, boxMax.y);

        if (!additive)
            ClearSelection();

        auto view = m_Scene->GetAllEntitiesWith<VE::TransformComponent>();
        VE::Entity lastHit;
        for (auto entityID : view) {
            ImVec2 screenPos = ProjectEntityToScreen(entityID);
            if (screenPos.x >= minX && screenPos.x <= maxX &&
                screenPos.y >= minY && screenPos.y <= maxY) {
                m_SelectedEntities.insert(entityID);
                lastHit = VE::Entity(entityID, &*m_Scene);
            }
        }
        if (lastHit)
            m_SelectedEntity = lastHit;
        m_SelectedAssetPath.clear();
    }


    // ── Main Menu Bar ──────────────────────────────────────────────────

    void DrawMainMenuBar() {
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("New Scene", "Ctrl+N")) NewScene();
                if (ImGui::MenuItem("Open Scene", "Ctrl+O")) LoadScene();
                if (ImGui::MenuItem("Load Additive Scene...")) LoadSceneAdditive();
                ImGui::Separator();
                if (ImGui::MenuItem("Save Scene", "Ctrl+S")) SaveScene();
                if (ImGui::MenuItem("Save Scene As...", "Ctrl+Shift+S")) SaveSceneAs();
                ImGui::Separator();
                if (ImGui::MenuItem("Create LOD Test Scene")) CreateLODTestScene();
                ImGui::Separator();
                if (ImGui::MenuItem("Build Settings...")) m_ShowBuildPanel = true;
                ImGui::Separator();
                if (ImGui::MenuItem("Exit"))
                    glfwSetWindowShouldClose(GetWindow().GetNativeWindow(), true);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Edit")) {
                if (ImGui::MenuItem("Undo", "Ctrl+Z", false, m_CommandHistory.CanUndo()))
                    m_CommandHistory.Undo();
                if (ImGui::MenuItem("Redo", "Ctrl+Y", false, m_CommandHistory.CanRedo()))
                    m_CommandHistory.Redo();
                ImGui::Separator();
                if (ImGui::MenuItem("Copy", "Ctrl+C", false, m_SelectedEntity.GetHandle() != entt::null))
                    CopySelectedEntity();
                if (ImGui::MenuItem("Cut", "Ctrl+X", false, m_SelectedEntity.GetHandle() != entt::null))
                    CutSelectedEntity();
                if (ImGui::MenuItem("Paste", "Ctrl+V", false, !m_EntityClipboard.empty()))
                    PasteEntity();
                ImGui::Separator();
                if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, m_SelectedEntity.GetHandle() != entt::null))
                    DuplicateSelectedEntity();
                if (ImGui::MenuItem("Delete", "Del", false, m_SelectedEntity.GetHandle() != entt::null || !m_SelectedEntities.empty()))
                    DeleteSelectedEntities();
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Settings")) {
                ImGui::MenuItem("Project Settings", nullptr, &m_ShowProjectSettings);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Lighting")) {
                if (ImGui::MenuItem("Bake All Reflection Probes"))
                    m_Scene->BakeReflectionProbes();
                if (ImGui::MenuItem("Bake Light Probes")) {
                    // Bake all light probes in the scene
                    auto probeView = m_Scene->GetAllEntitiesWith<VE::TransformComponent, VE::LightProbeComponent>();
                    int bakedCount = 0;
                    for (auto e : probeView) {
                        auto& tc = probeView.get<VE::TransformComponent>(e);
                        auto& lpc = probeView.get<VE::LightProbeComponent>(e);
                        glm::vec3 pos(tc.Position[0], tc.Position[1], tc.Position[2]);
                        VE::LightProbe probe;
                        probe.Bake(*m_Scene, pos, static_cast<uint32_t>(lpc.Resolution));
                        auto coeffs = probe.GetCoefficients();
                        for (int i = 0; i < 9; ++i) lpc.SHCoefficients[i] = coeffs[i];
                        lpc.IsBaked = true;
                        bakedCount++;
                    }
                    VE_INFO("Baked {} light probes", bakedCount);
                }
                if (ImGui::MenuItem("Bake Lightmaps")) {
                    // Bake lightmaps for all entities with LightmapComponent + IsStatic
                    auto& reg = m_Scene->GetRegistry();
                    auto view = reg.view<VE::TransformComponent, VE::MeshRendererComponent, VE::LightmapComponent>();
                    int bakedCount = 0;
                    for (auto e : view) {
                        auto& lmc = view.get<VE::LightmapComponent>(e);
                        if (!lmc.IsStatic) continue;

                        auto& mr = view.get<VE::MeshRendererComponent>(e);
                        if (!mr.Mesh) continue;

                        glm::mat4 model = m_Scene->GetWorldTransform(e);

                        // Read vertex data from GPU
                        auto vbs = mr.Mesh->GetVertexBuffers();
                        if (vbs.empty()) continue;
                        auto& vb = vbs[0];
                        uint32_t vbSize = vb->GetSize();
                        if (vbSize == 0) continue;

                        std::vector<float> vertexData(vbSize / sizeof(float));
                        vb->Bind();
                        glGetBufferSubData(GL_ARRAY_BUFFER, 0, vbSize, vertexData.data());

                        auto ib = mr.Mesh->GetIndexBuffer();
                        if (!ib) continue;
                        uint32_t indexCount = ib->GetCount();
                        std::vector<uint32_t> indexData(indexCount);
                        ib->Bind();
                        glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, indexCount * sizeof(uint32_t), indexData.data());

                        auto lightmap = VE::Lightmapper::BakeEntity(
                            *m_Scene, model, vertexData, indexData,
                            lmc.Resolution, 0.03f);

                        if (lightmap) {
                            lmc.LightmapTexture = lightmap;
                            bakedCount++;
                        }
                    }
                    VE_INFO("Baked {} lightmaps", bakedCount);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Bake All")) {
                    // Bake both probes and lightmaps
                    auto probeView = m_Scene->GetAllEntitiesWith<VE::TransformComponent, VE::LightProbeComponent>();
                    for (auto e : probeView) {
                        auto& tc = probeView.get<VE::TransformComponent>(e);
                        auto& lpc = probeView.get<VE::LightProbeComponent>(e);
                        glm::vec3 pos(tc.Position[0], tc.Position[1], tc.Position[2]);
                        VE::LightProbe probe;
                        probe.Bake(*m_Scene, pos, static_cast<uint32_t>(lpc.Resolution));
                        auto coeffs = probe.GetCoefficients();
                        for (int i = 0; i < 9; ++i) lpc.SHCoefficients[i] = coeffs[i];
                        lpc.IsBaked = true;
                    }

                    auto& reg = m_Scene->GetRegistry();
                    auto lmView = reg.view<VE::TransformComponent, VE::MeshRendererComponent, VE::LightmapComponent>();
                    for (auto e : lmView) {
                        auto& lmc = lmView.get<VE::LightmapComponent>(e);
                        if (!lmc.IsStatic) continue;
                        auto& mr = lmView.get<VE::MeshRendererComponent>(e);
                        if (!mr.Mesh) continue;

                        glm::mat4 model = m_Scene->GetWorldTransform(e);
                        auto vbs = mr.Mesh->GetVertexBuffers();
                        if (vbs.empty()) continue;
                        auto& vb = vbs[0];
                        uint32_t vbSize = vb->GetSize();
                        if (vbSize == 0) continue;

                        std::vector<float> vertexData(vbSize / sizeof(float));
                        vb->Bind();
                        glGetBufferSubData(GL_ARRAY_BUFFER, 0, vbSize, vertexData.data());

                        auto ib = mr.Mesh->GetIndexBuffer();
                        if (!ib) continue;
                        uint32_t indexCount = ib->GetCount();
                        std::vector<uint32_t> indexData(indexCount);
                        ib->Bind();
                        glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, indexCount * sizeof(uint32_t), indexData.data());

                        auto lightmap = VE::Lightmapper::BakeEntity(
                            *m_Scene, model, vertexData, indexData,
                            lmc.Resolution, 0.03f);
                        if (lightmap) lmc.LightmapTexture = lightmap;
                    }
                    VE_INFO("Bake All: light probes and lightmaps complete");
                }
                if (ImGui::MenuItem("Clear All Baked Data")) {
                    auto probeView2 = m_Scene->GetAllEntitiesWith<VE::LightProbeComponent>();
                    for (auto e : probeView2) {
                        auto& lpc = probeView2.get<VE::LightProbeComponent>(e);
                        lpc.IsBaked = false;
                        for (auto& c : lpc.SHCoefficients) c = glm::vec3(0.0f);
                    }
                    auto lmView2 = m_Scene->GetAllEntitiesWith<VE::LightmapComponent>();
                    for (auto e : lmView2) {
                        auto& lmc = lmView2.get<VE::LightmapComponent>(e);
                        lmc.LightmapTexture = nullptr;
                    }
                    VE_INFO("Cleared all baked lighting data");
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("GameObject")) {
                if (ImGui::MenuItem("Create Empty"))
                    m_Scene->CreateEntity("GameObject");
                if (ImGui::MenuItem("Create Triangle")) {
                    auto e = m_Scene->CreateEntity("Triangle");
                    auto& mr = e.AddComponent<VE::MeshRendererComponent>();
                    mr.Mesh = VE::MeshLibrary::GetTriangle();
                    mr.Mat = VE::MaterialLibrary::Get("Default");
                }
                if (ImGui::MenuItem("Create Quad")) {
                    auto e = m_Scene->CreateEntity("Quad");
                    auto& mr = e.AddComponent<VE::MeshRendererComponent>();
                    mr.Mesh = VE::MeshLibrary::GetQuad();
                    mr.Mat = VE::MaterialLibrary::Get("Default");
                }
                if (ImGui::MenuItem("Create Cube")) {
                    auto e = m_Scene->CreateEntity("Cube");
                    auto& mr = e.AddComponent<VE::MeshRendererComponent>();
                    mr.Mesh = VE::MeshLibrary::GetCube();
                    mr.Mat = VE::MaterialLibrary::Get("Lit");
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Select All", "Ctrl+A")) {
                    auto& reg = m_Scene->GetRegistry();
                    auto view = reg.view<VE::TagComponent>();
                    ClearSelection();
                    for (auto e : view) {
                        m_SelectedEntities.insert(e);
                        m_SelectedEntity = VE::Entity(e, &*m_Scene);
                    }
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("GameObject")) {
                if (ImGui::MenuItem("Create Empty"))
                    m_CommandHistory.Execute("Create Entity", [this]() { m_Scene->CreateEntity("GameObject"); });
                if (ImGui::MenuItem("Create Triangle"))
                    m_CommandHistory.Execute("Create Triangle", [this]() {
                        auto e = m_Scene->CreateEntity("Triangle");
                        auto& mr = e.AddComponent<VE::MeshRendererComponent>();
                        mr.Mesh = VE::MeshLibrary::GetTriangle();
                        mr.Mat = VE::MaterialLibrary::Get("Default");
                    });
                if (ImGui::MenuItem("Create Quad"))
                    m_CommandHistory.Execute("Create Quad", [this]() {
                        auto e = m_Scene->CreateEntity("Quad");
                        auto& mr = e.AddComponent<VE::MeshRendererComponent>();
                        mr.Mesh = VE::MeshLibrary::GetQuad();
                        mr.Mat = VE::MaterialLibrary::Get("Default");
                    });
                if (ImGui::MenuItem("Create Cube"))
                    m_CommandHistory.Execute("Create Cube", [this]() {
                        auto e = m_Scene->CreateEntity("Cube");
                        auto& mr = e.AddComponent<VE::MeshRendererComponent>();
                        mr.Mesh = VE::MeshLibrary::GetCube();
                        mr.Mat = VE::MaterialLibrary::Get("Lit");
                    });
                ImGui::Separator();
                if (ImGui::MenuItem("Create Directional Light"))
                    m_CommandHistory.Execute("Create Light", [this]() {
                        auto e = m_Scene->CreateEntity("Directional Light");
                        e.GetComponent<VE::TransformComponent>().Rotation =
                            EulerFromForwardDirection(glm::vec3(0.3f, 1.0f, 0.5f));
                        e.AddComponent<VE::DirectionalLightComponent>();
                    });
                if (ImGui::MenuItem("Create Light Probe"))
                    m_CommandHistory.Execute("Create Light Probe", [this]() {
                        auto e = m_Scene->CreateEntity("Light Probe");
                        e.AddComponent<VE::LightProbeComponent>();
                    });
                if (ImGui::MenuItem("Create Decal"))
                    m_CommandHistory.Execute("Create Decal", [this]() {
                        auto e = m_Scene->CreateEntity("Decal");
                        e.AddComponent<VE::DecalComponent>();
                    });
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                ImGui::MenuItem("Hierarchy", nullptr, &m_ShowHierarchy);
                ImGui::MenuItem("Inspector", nullptr, &m_ShowInspector);
                ImGui::MenuItem("Scene Info", nullptr, &m_ShowSceneInfo);
                ImGui::Separator();
                ImGui::MenuItem("Render Pipeline", nullptr, &m_ShowPipelineSettings);
                ImGui::MenuItem("Render Debugger", nullptr, &m_ShowRenderDebugger);
                ImGui::MenuItem("Scripting", nullptr, &m_ShowScripting);
                ImGui::MenuItem("Content Browser", nullptr, &m_ShowContentBrowser);
                ImGui::MenuItem("Game", nullptr, &m_ShowGameView);
                ImGui::MenuItem("Input Settings", nullptr, &m_ShowInputSettings);
                ImGui::MenuItem("Material Editor", nullptr, &m_ShowMaterialEditor);

                ImGui::Separator();
                ImGui::MenuItem("Profiler", nullptr, &m_ShowProfiler);
                ImGui::MenuItem("Show Colliders", nullptr, &m_ShowColliders);

                ImGui::MenuItem("Scene Manager", nullptr, &m_ShowSceneManager);
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        ImGuiIO& io = ImGui::GetIO();
        bool ctrl = io.KeyCtrl, shift = io.KeyShift;
        if (ctrl && !shift && ImGui::IsKeyPressed(ImGuiKey_N)) NewScene();
        if (ctrl && !shift && ImGui::IsKeyPressed(ImGuiKey_O)) LoadScene();
        if (ctrl && !shift && ImGui::IsKeyPressed(ImGuiKey_S)) SaveScene();
        if (ctrl &&  shift && ImGui::IsKeyPressed(ImGuiKey_S)) SaveSceneAs();
        if (ctrl && !shift && ImGui::IsKeyPressed(ImGuiKey_R, false))
            VE::ScriptEngine::RequestBuildAndReload();
        if (ctrl &&  shift && ImGui::IsKeyPressed(ImGuiKey_R, false))
            VE::PluginEngine::RequestBuildAndReload();
    }

    // ── Viewport Panel ──────────────────────────────────────────────────

    void DrawViewportPanel() {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("Scene Viewport");

        m_ViewportHovered = ImGui::IsWindowHovered();
        m_ViewportFocused = ImGui::IsWindowFocused();

        ImVec2 viewportSize = ImGui::GetContentRegionAvail();
        if (viewportSize.x > 0 && viewportSize.y > 0) {
            uint32_t w = static_cast<uint32_t>(viewportSize.x);
            uint32_t h = static_cast<uint32_t>(viewportSize.y);
            if (m_Framebuffer)
                m_Framebuffer->Resize(w, h);
            m_Camera.SetViewportSize(viewportSize.x, viewportSize.y);
        }

        // Display the framebuffer texture (post-processed if bloom is active)
        if (m_Framebuffer) {
            uint64_t texID = (m_PostProcessedTexture != 0)
                ? static_cast<uint64_t>(m_PostProcessedTexture)
                : m_Framebuffer->GetColorAttachmentID();
            if (texID != 0) {
                ImGui::Image((ImTextureID)texID, viewportSize, ImVec2(0, 1), ImVec2(1, 0));
            }
        }

        // Drag-drop target: drop mesh into viewport to create entity
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_MESH")) {
                std::string path(static_cast<const char*>(payload->Data));
                auto meshAsset = VE::MeshImporter::GetOrLoad(path);
                if (meshAsset && meshAsset->VAO) {
                    auto vao = meshAsset->VAO;
                    m_CommandHistory.Execute("Import Mesh", [this, vao, path]() {
                        auto entity = m_Scene->CreateEntity("Imported Mesh");
                        auto& mr = entity.AddComponent<VE::MeshRendererComponent>();
                        mr.Mesh = vao;
                        mr.Mat = VE::MaterialLibrary::Get("Lit");
                        mr.MeshSourcePath = path;
                    });
                }
            }
            ImGui::EndDragDropTarget();
        }

        // Gizmos are drawn as overlay on the viewport using ImDrawList
        ImVec2 vpMin = ImGui::GetWindowContentRegionMin();
        ImVec2 vpMax = ImGui::GetWindowContentRegionMax();
        ImVec2 wPos  = ImGui::GetWindowPos();
        float vpX = wPos.x + vpMin.x;
        float vpY = wPos.y + vpMin.y;
        float vpW = vpMax.x - vpMin.x;
        float vpH = vpMax.y - vpMin.y;
        m_SceneVpX = vpX; m_SceneVpY = vpY; m_SceneVpW = vpW; m_SceneVpH = vpH;

        // Viewport toggle buttons (top-right corner)
        {
            float btnSize = 24.0f;
            float padding = 8.0f;
            float spacing = 4.0f;

            // Helper lambda for toggle buttons
            auto DrawToggleButton = [&](const char* label, bool& state, const char* tooltip, float xOffset) {
                ImVec2 btnPos(vpX + vpW - xOffset, vpY + padding);
                ImGui::SetCursorScreenPos(btnPos);
                ImVec4 btnColor = state
                    ? ImVec4(0.2f, 0.4f, 0.9f, 1.0f)
                    : ImVec4(0.4f, 0.4f, 0.4f, 1.0f);
                ImVec4 btnHover = state
                    ? ImVec4(0.3f, 0.5f, 1.0f, 1.0f)
                    : ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
                ImGui::PushStyleColor(ImGuiCol_Button, btnColor);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, btnHover);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, btnColor);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
                if (ImGui::Button(label, ImVec2(btnSize, btnSize)))
                    state = !state;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s: %s", tooltip, state ? "ON" : "OFF");
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(3);
            };

            // Right to left: Gizmos, Outline, FPS
            DrawToggleButton("G##GizmosToggle",  m_GizmosEnabled, "Gizmos",  btnSize + padding);
            DrawToggleButton("O##OutlineToggle", m_OutlineEnabled, "Outline", (btnSize + spacing) * 2 + padding - spacing);
        }

        if (m_GizmosEnabled) {
            VE::GizmoRenderer::BeginScene(m_FrameVP, vpX, vpY, vpW, vpH, m_Camera.GetMode(),
                ImGui::GetWindowDrawList());
            VE::GizmoRenderer::DrawGrid(20.0f, 1.0f);

            // Draw point light gizmos
            {
                auto plView = m_Scene->GetAllEntitiesWith<VE::TransformComponent, VE::PointLightComponent>();
                for (auto e : plView) {
                    glm::mat4 wm = m_Scene->GetWorldTransform(e);
                    glm::vec3 pos = glm::vec3(wm[3]);
                    auto& pl = plView.get<VE::PointLightComponent>(e);
                    VE::GizmoRenderer::DrawPointLightGizmo(pos, pl.Range,
                        glm::vec3(pl.Color[0], pl.Color[1], pl.Color[2]));
                }
            }

            // Draw spot light gizmos
            {
                auto slView = m_Scene->GetAllEntitiesWith<VE::TransformComponent, VE::SpotLightComponent>();
                for (auto e : slView) {
                    glm::mat4 wm = m_Scene->GetWorldTransform(e);
                    glm::vec3 pos = glm::vec3(wm[3]);
                    auto& sl = slView.get<VE::SpotLightComponent>(e);
                    glm::vec3 localDir = glm::normalize(glm::vec3(sl.Direction[0], sl.Direction[1], sl.Direction[2]));
                    glm::vec3 worldDir = glm::normalize(glm::mat3(wm) * localDir);
                    VE::GizmoRenderer::DrawSpotLightGizmo(pos, worldDir, sl.Range, sl.OuterAngle,
                        glm::vec3(sl.Color[0], sl.Color[1], sl.Color[2]));
                }
            }

            // Draw reflection probe influence boxes
            {
                auto rpView = m_Scene->GetAllEntitiesWith<VE::TransformComponent, VE::ReflectionProbeComponent>();
                for (auto e : rpView) {
                    auto& rp = rpView.get<VE::ReflectionProbeComponent>(e);
                    glm::mat4 wm = m_Scene->GetWorldTransform(e);
                    // Scale the wireframe box by the BoxSize
                    glm::mat4 boxMat = wm * glm::scale(glm::mat4(1.0f),
                        glm::vec3(rp.BoxSize[0], rp.BoxSize[1], rp.BoxSize[2]));
                    VE::GizmoRenderer::DrawWireframeBox(boxMat);
                }
            }

            // Draw camera frustum gizmos
            {
                auto camGizView = m_Scene->GetAllEntitiesWith<VE::TransformComponent, VE::CameraComponent>();
                for (auto e : camGizView) {
                    auto& cam = camGizView.get<VE::CameraComponent>(e);
                    glm::mat4 wm = m_Scene->GetWorldTransform(e);
                    float aspect = m_Framebuffer
                        ? static_cast<float>(m_Framebuffer->GetWidth()) / static_cast<float>(m_Framebuffer->GetHeight())
                        : 16.0f / 9.0f;
                    VE::GizmoRenderer::DrawCameraFrustum(wm,
                        static_cast<int>(cam.ProjectionType), cam.FOV, cam.Size,
                        cam.NearClip, cam.FarClip, aspect);
                }
            }

            // Draw collider wireframes for selected entity
            if (m_ShowColliders) {
                auto drawColliderForEntity = [&](entt::entity e) {
                    glm::mat4 wm = m_Scene->GetWorldTransform(e);
                    glm::vec3 pos = glm::vec3(wm[3]);
                    glm::vec3 scale(glm::length(glm::vec3(wm[0])),
                                    glm::length(glm::vec3(wm[1])),
                                    glm::length(glm::vec3(wm[2])));
                    glm::mat3 rot = glm::mat3(wm);
                    rot[0] = glm::normalize(rot[0]);
                    rot[1] = glm::normalize(rot[1]);
                    rot[2] = glm::normalize(rot[2]);

                    auto& reg = m_Scene->GetRegistry();

                    if (auto* box = reg.try_get<VE::BoxColliderComponent>(e)) {
                        glm::vec3 offset(box->Offset[0], box->Offset[1], box->Offset[2]);
                        glm::vec3 center = pos + rot * (offset * scale);
                        glm::vec3 size(box->Size[0] * scale.x, box->Size[1] * scale.y, box->Size[2] * scale.z);
                        VE::GizmoRenderer::DrawWireframeBoxCollider(center, size, rot);
                    }
                    if (auto* sphere = reg.try_get<VE::SphereColliderComponent>(e)) {
                        glm::vec3 offset(sphere->Offset[0], sphere->Offset[1], sphere->Offset[2]);
                        glm::vec3 center = pos + rot * (offset * scale);
                        float maxScale = std::max({ scale.x, scale.y, scale.z });
                        VE::GizmoRenderer::DrawWireframeSphereCollider(center, sphere->Radius * maxScale);
                    }
                    if (auto* capsule = reg.try_get<VE::CapsuleColliderComponent>(e)) {
                        glm::vec3 offset(capsule->Offset[0], capsule->Offset[1], capsule->Offset[2]);
                        glm::vec3 center = pos + rot * (offset * scale);
                        float maxScale = std::max({ scale.x, scale.y, scale.z });
                        VE::GizmoRenderer::DrawWireframeCapsuleCollider(center,
                            capsule->Radius * maxScale, capsule->Height * maxScale, rot);
                    }
                };

                if (m_SelectedEntity && m_SelectedEntity.HasComponent<VE::TransformComponent>()) {
                    drawColliderForEntity(m_SelectedEntity.GetHandle());
                }
            }

            // Draw decal volume wireframe gizmos
            {
                auto decalView = m_Scene->GetAllEntitiesWith<VE::TransformComponent, VE::DecalComponent>();
                for (auto e : decalView) {
                    auto& dc = decalView.get<VE::DecalComponent>(e);
                    glm::mat4 wm = m_Scene->GetWorldTransform(e);
                    glm::mat4 sizeScale = glm::scale(glm::mat4(1.0f),
                        glm::vec3(dc.Size[0], dc.Size[1], dc.Size[2]));
                    VE::GizmoRenderer::DrawWireframeBox(wm * sizeScale);
                }
            }

            // Draw selection wireframe for all selected entities (except primary, which gets gizmo)
            for (auto selEntity : m_SelectedEntities) {
                if (selEntity == m_SelectedEntity.GetHandle()) continue;
                if (!m_Scene->GetRegistry().valid(selEntity)) continue;
                if (!m_Scene->GetRegistry().any_of<VE::TransformComponent>(selEntity)) continue;
                glm::mat4 wm = m_Scene->GetWorldTransform(selEntity);
                VE::GizmoRenderer::DrawWireframeBox(wm);
            }


            if (m_SelectedEntity && m_SelectedEntity.HasComponent<VE::TransformComponent>()) {
                glm::mat4 worldMat = m_Scene->GetWorldTransform(m_SelectedEntity.GetHandle());

                glm::vec3 worldPos = glm::vec3(worldMat[3]);
                glm::mat3 worldRot = glm::mat3(worldMat);
                worldRot[0] = glm::normalize(worldRot[0]);
                worldRot[1] = glm::normalize(worldRot[1]);
                worldRot[2] = glm::normalize(worldRot[2]);

                VE::GizmoAxis displayAxis = m_DraggingAxis;
                ImGuiIO& io = ImGui::GetIO();
                if (displayAxis == VE::GizmoAxis::None && m_ViewportHovered) {
                    if (m_GizmoTool == SceneGizmoTool::Rotate) {
                        displayAxis = VE::GizmoRenderer::HitTestRotationGizmo(
                            worldPos, io.MousePos.x, io.MousePos.y, 12.0f, worldRot);
                    } else {
                        displayAxis = VE::GizmoRenderer::HitTestTranslationGizmo(
                            worldPos, io.MousePos.x, io.MousePos.y, 12.0f, worldRot);
                    }
                }
                if (m_GizmoTool == SceneGizmoTool::Rotate)
                    VE::GizmoRenderer::DrawRotationGizmo(m_SelectedEntity, displayAxis, worldMat);
                else
                    VE::GizmoRenderer::DrawTranslationGizmo(m_SelectedEntity, displayAxis, worldMat);
            }

            // Draw IK target gizmos for selected entity
            if (m_SelectedEntity.IsValid() && m_SelectedEntity.HasComponent<VE::IKComponent>()) {
                auto& ik = m_SelectedEntity.GetComponent<VE::IKComponent>();
                auto& tc = m_SelectedEntity.GetComponent<VE::TransformComponent>();
                glm::vec3 entityPos(tc.Position[0], tc.Position[1], tc.Position[2]);

                for (int i = 0; i < static_cast<int>(ik.Targets.size()); i++) {
                    auto& t = ik.Targets[i];
                    if (!t.Enabled) continue;

                    glm::vec3 tgtPos = t.TargetPosition;

                    // Draw line from entity position to IK target
                    glm::vec2 p0 = VE::GizmoRenderer::ProjectWorldToScreen(entityPos);
                    glm::vec2 p1 = VE::GizmoRenderer::ProjectWorldToScreen(tgtPos);
                    ImU32 col = IM_COL32(255, 165, 0, 200); // orange
                    ImGui::GetWindowDrawList()->AddLine(ImVec2(p0.x, p0.y), ImVec2(p1.x, p1.y), col, 1.5f);

                    // Draw dot at target position
                    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p1.x, p1.y), 5.0f, col);

                    // Draw dot at pole vector position
                    glm::vec2 pp = VE::GizmoRenderer::ProjectWorldToScreen(t.PoleVector);
                    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(pp.x, pp.y), 3.0f, IM_COL32(0, 200, 255, 180));
                }
            }

            VE::GizmoRenderer::EndScene();
        }

        // Draw viewport box selection rectangle
        if (m_EntityBoxSelecting) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 bMin(std::min(m_EntityBoxSelectStart.x, m_EntityBoxSelectEnd.x),
                        std::min(m_EntityBoxSelectStart.y, m_EntityBoxSelectEnd.y));
            ImVec2 bMax(std::max(m_EntityBoxSelectStart.x, m_EntityBoxSelectEnd.x),
                        std::max(m_EntityBoxSelectStart.y, m_EntityBoxSelectEnd.y));
            dl->AddRectFilled(bMin, bMax, IM_COL32(80, 140, 230, 40));
            dl->AddRect(bMin, bMax, IM_COL32(80, 140, 230, 180), 0.0f, 0, 1.5f);
        }

        ImGui::End();
        ImGui::PopStyleVar();
    }

    // ── Toolbar ────────────────────────────────────────────────────────

    void EnterPlayMode() {
        if (m_PlayMode) return;

        // Auto-build scripts if DLL not loaded
        if (!VE::ScriptEngine::IsLoaded() && VE::ScriptEngine::HasScriptFiles()) {
            VE_INFO("Auto-building scripts before Play...");
            VE::ScriptEngine::BuildScriptProjectSync();
        }

        // Save scene snapshots (all loaded scenes via SceneManager)
        m_SceneManagerSnapshots = m_SceneManager.SnapshotAll();

        // Also keep legacy single-scene snapshot for backward compat
        VE::SceneSerializer serializer(m_Scene);
        m_SceneSnapshot = serializer.SerializeToString();

        // Start all subsystems on all loaded scenes
        m_SceneManager.StartAllPhysics();
        VE::ScriptEngine::SetActiveScene(m_Scene.get());
        m_SceneManager.StartAllScripts();
        m_SceneManager.StartAllAnimations();
        m_SceneManager.StartAllSpriteAnimations();
        m_SceneManager.StartAllAudio();
        m_SceneManager.StartAllParticles();
        m_PlayMode = true;
        VE::ScriptEngine::SetPlayMode(true);
        m_CommandHistory.Clear();
        VE_INFO("Entered Play mode");
    }

    void ExitPlayMode() {
        if (!m_PlayMode) return;
        m_Paused = false;
        m_StepOneFrame = false;

        // Stop all subsystems on all loaded scenes
        m_SceneManager.StopAllParticles();
        m_SceneManager.StopAllAudio();
        m_SceneManager.StopAllSpriteAnimations();
        m_SceneManager.StopAllAnimations();
        m_SceneManager.StopAllScripts();
        m_SceneManager.StopAllPhysics();

        // Restore all scenes from snapshots
        if (!m_SceneManagerSnapshots.empty()) {
            m_SceneManager.RestoreAll(m_SceneManagerSnapshots);
            m_Scene = m_SceneManager.GetActiveScene();
            m_SceneManagerSnapshots.clear();
        } else {
            // Legacy fallback: restore single scene
            m_Scene = std::make_shared<VE::Scene>();
            VE::SceneSerializer serializer(m_Scene);
            serializer.DeserializeFromString(m_SceneSnapshot);
            m_SceneManager.UnloadAllScenes();
            m_SceneManager.AddScene(m_Scene, "Untitled");
        }


        // Restore scene from snapshot
        m_Scene = std::make_shared<VE::Scene>();
        ClearSelection();
        VE::SceneSerializer serializer(m_Scene);
        serializer.DeserializeFromString(m_SceneSnapshot);

        m_SelectedEntity = {};
        m_SceneSnapshot.clear();

        m_PlayMode = false;
        VE::ScriptEngine::SetPlayMode(false);
        m_CommandHistory.Clear();
        VE_INFO("Exited Play mode (scene restored)");
    }

    void DrawToolbar() {
        // Unity-style centered toolbar strip below menu bar
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        float menuBarHeight = ImGui::GetFrameHeight(); // main menu bar height
        float toolbarHeight = 32.0f;

        ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + menuBarHeight));
        ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, toolbarHeight));

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
            | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings;

        // Toolbar background — tinted red during play mode
        if (m_PlayMode)
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.32f, 0.12f, 0.12f, 1.0f));
        else
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.18f, 0.18f, 0.18f, 1.0f));

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

        ImGui::Begin("##Toolbar", nullptr, flags);

        float btnH = toolbarHeight - 8.0f; // button height
        float btnW = btnH;                 // square buttons
        float spacing = 2.0f;

        // Center the button group
        float totalW = btnW * 3 + spacing * 2; // Play + Pause + Step
        float startX = (viewport->Size.x - totalW) * 0.5f;
        ImGui::SetCursorPosX(startX);
        ImGui::SetCursorPosY((toolbarHeight - btnH) * 0.5f);

        // ── Play / Stop button ──
        bool wasPlaying = m_PlayMode;
        if (wasPlaying) {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.2f, 0.5f, 0.8f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.3f, 0.6f, 0.9f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.15f, 0.4f, 0.7f, 1.0f));
        }
        const char* playLabel = wasPlaying ? "||" : ">";
        if (ImGui::Button(playLabel, ImVec2(btnW, btnH))) {
            if (wasPlaying)
                ExitPlayMode();
            else
                EnterPlayMode();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(wasPlaying ? "Stop (exit play mode)" : "Play");
        if (wasPlaying)
            ImGui::PopStyleColor(3);

        // ── Pause button ──
        ImGui::SameLine(0, spacing);
        bool paused = m_PlayMode && m_Paused;
        if (paused) {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.6f, 0.5f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.7f, 0.6f, 0.3f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.5f, 0.4f, 0.15f, 1.0f));
        }
        if (ImGui::Button("||##Pause", ImVec2(btnW, btnH)) && m_PlayMode)
            m_Paused = !m_Paused;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Pause");
        if (paused)
            ImGui::PopStyleColor(3);

        // ── Step button ──
        ImGui::SameLine(0, spacing);
        if (ImGui::Button(">|", ImVec2(btnW, btnH)) && m_PlayMode) {
            m_Paused = false;
            m_StepOneFrame = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Step (advance one frame)");

        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }

    // ── Hierarchy Panel ────────────────────────────────────────────────

    void DrawHierarchyPanel() {
        if (!m_ShowHierarchy) return;
        ImGui::Begin("Hierarchy", &m_ShowHierarchy);

        // Draw only root entities (no parent), children are drawn recursively
        // Collect roots in creation order (entt iterates newest-first, so reverse)
        auto view = m_Scene->GetAllEntitiesWith<VE::RelationshipComponent>();
        std::vector<entt::entity> roots;
        for (auto entityID : view) {
            auto& rel = view.get<VE::RelationshipComponent>(entityID);
            if (rel.Parent == entt::null)
                roots.push_back(entityID);
        }
        std::reverse(roots.begin(), roots.end());

        std::string sceneLabel = GetCurrentSceneDisplayName();
        if (m_SceneDirty)
            sceneLabel += " *";

        ImGuiTreeNodeFlags sceneFlags = ImGuiTreeNodeFlags_OpenOnArrow |
            ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
        bool sceneOpen = ImGui::TreeNodeEx("##CurrentSceneRoot", sceneFlags, "%s", sceneLabel.c_str());
        if (ImGui::IsItemHovered() && !m_CurrentScenePath.empty())
            ImGui::SetTooltip("%s", m_CurrentScenePath.c_str());

        if (sceneOpen) {
            for (auto entityID : roots)
                DrawEntityNode(entityID);
            ImGui::TreePop();
        }

        // Drop on empty space: unparent
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENTITY_NODE")) {
                entt::entity dropped = *static_cast<const entt::entity*>(payload->Data);
                m_CommandHistory.Execute("Reparent Entity", [this, dropped]() {
                    m_Scene->RemoveParent(dropped);
                });
            }
            ImGui::EndDragDropTarget();
        }

        if (ImGui::BeginPopupContextWindow("HierarchyPopup",
                ImGuiPopupFlags_NoOpenOverItems | ImGuiPopupFlags_MouseButtonRight)) {
            if (ImGui::BeginMenu("Create")) {
                if (ImGui::MenuItem("Empty"))
                    m_CommandHistory.Execute("Create Entity", [this]() {
                        SelectEntityOnly(m_Scene->CreateEntity("GameObject"));
                    });
                ImGui::Separator();
                if (ImGui::MenuItem("Triangle"))
                    m_CommandHistory.Execute("Create Triangle", [this]() {
                        auto e = m_Scene->CreateEntity("Triangle");
                        auto& mr = e.AddComponent<VE::MeshRendererComponent>();
                        mr.Mesh = VE::MeshLibrary::GetTriangle();
                        mr.Mat = VE::MaterialLibrary::Get("Default");
                        SelectEntityOnly(e);
                    });
                if (ImGui::MenuItem("Quad"))
                    m_CommandHistory.Execute("Create Quad", [this]() {
                        auto e = m_Scene->CreateEntity("Quad");
                        auto& mr = e.AddComponent<VE::MeshRendererComponent>();
                        mr.Mesh = VE::MeshLibrary::GetQuad();
                        mr.Mat = VE::MaterialLibrary::Get("Default");
                        SelectEntityOnly(e);
                    });
                if (ImGui::MenuItem("Cube"))
                    m_CommandHistory.Execute("Create Cube", [this]() {
                        auto e = m_Scene->CreateEntity("Cube");
                        auto& mr = e.AddComponent<VE::MeshRendererComponent>();
                        mr.Mesh = VE::MeshLibrary::GetCube();
                        mr.Mat = VE::MaterialLibrary::Get("Lit");
                        SelectEntityOnly(e);
                    });
                if (ImGui::MenuItem("Sphere"))
                    m_CommandHistory.Execute("Create Sphere", [this]() {
                        auto e = m_Scene->CreateEntity("Sphere");
                        auto& mr = e.AddComponent<VE::MeshRendererComponent>();
                        mr.Mesh = VE::MeshLibrary::GetSphere();
                        mr.Mat = VE::MaterialLibrary::Get("Lit");
                        mr.LocalBounds = VE::MeshLibrary::GetMeshAABB(3);
                        SelectEntityOnly(e);
                    });
                ImGui::Separator();
                if (ImGui::MenuItem("Directional Light"))
                    m_CommandHistory.Execute("Create Light", [this]() {
                        auto e = m_Scene->CreateEntity("Directional Light");
                        e.GetComponent<VE::TransformComponent>().Rotation =
                            EulerFromForwardDirection(glm::vec3(0.3f, 1.0f, 0.5f));
                        e.AddComponent<VE::DirectionalLightComponent>();
                        SelectEntityOnly(e);
                    });
                if (ImGui::MenuItem("Point Light"))
                    m_CommandHistory.Execute("Create Point Light", [this]() {
                        auto e = m_Scene->CreateEntity("Point Light");
                        e.AddComponent<VE::PointLightComponent>();
                        SelectEntityOnly(e);
                    });
                if (ImGui::MenuItem("Spot Light"))
                    m_CommandHistory.Execute("Create Spot Light", [this]() {
                        auto e = m_Scene->CreateEntity("Spot Light");
                        e.AddComponent<VE::SpotLightComponent>();
                        m_SelectedEntity = e;
                    });
                if (ImGui::MenuItem("Area Light"))
                    m_CommandHistory.Execute("Create Area Light", [this]() {
                        auto e = m_Scene->CreateEntity("Area Light");
                        e.AddComponent<VE::AreaLightComponent>();
                        SelectEntityOnly(e);
                    });
                if (ImGui::MenuItem("Spot Light"))
                    m_CommandHistory.Execute("Create Spot Light", [this]() {
                        auto e = m_Scene->CreateEntity("Spot Light");
                        e.AddComponent<VE::SpotLightComponent>();
                        m_SelectedEntity = e;
                    });
                if (ImGui::MenuItem("Reflection Probe"))
                    m_CommandHistory.Execute("Create Reflection Probe", [this]() {
                        auto e = m_Scene->CreateEntity("Reflection Probe");
                        e.AddComponent<VE::ReflectionProbeComponent>();
                        m_SelectedEntity = e;
                    });
                if (ImGui::MenuItem("Light Probe"))
                    m_CommandHistory.Execute("Create Light Probe", [this]() {
                        auto e = m_Scene->CreateEntity("Light Probe");
                        e.AddComponent<VE::LightProbeComponent>();
                        m_SelectedEntity = e;
                    });
                if (ImGui::MenuItem("Camera"))
                    m_CommandHistory.Execute("Create Camera", [this]() {
                        auto e = m_Scene->CreateEntity("Camera");
                        e.AddComponent<VE::CameraComponent>();
                        SelectEntityOnly(e);
                    });
                ImGui::Separator();
                if (ImGui::MenuItem("Particle Emitter"))
                    m_CommandHistory.Execute("Create Particle Emitter", [this]() {
                        auto e = m_Scene->CreateEntity("Particle Emitter");
                        e.AddComponent<VE::ParticleSystemComponent>();
                        m_SelectedEntity = e;
                    });
                if (ImGui::MenuItem("Decal"))
                    m_CommandHistory.Execute("Create Decal", [this]() {
                        auto e = m_Scene->CreateEntity("Decal");
                        e.AddComponent<VE::DecalComponent>();
                        m_SelectedEntity = e;
                    });
                ImGui::EndMenu();
            }
            ImGui::EndPopup();
        }

        if ((m_SelectedEntity || !m_SelectedEntities.empty()) && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            DeleteSelectedEntities();
        }
        ImGui::End();
    }

    void DrawEntityNode(entt::entity entityID) {
        auto& registry = m_Scene->GetRegistry();
        if (!registry.valid(entityID)) return;

        auto& tag = registry.get<VE::TagComponent>(entityID);
        auto& rel = registry.get<VE::RelationshipComponent>(entityID);

        bool isSelected = IsEntitySelected(entityID);
        bool hasChildren = !rel.Children.empty();
        bool isActive = m_Scene->IsEntityActiveInHierarchy(entityID);

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (isSelected) flags |= ImGuiTreeNodeFlags_Selected;
        if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf;

        if (!isActive)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 0.6f));

        bool opened = ImGui::TreeNodeEx(
            reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<uint32_t>(entityID))),
            flags, "%s", tag.Tag.c_str());

        if (!isActive)
            ImGui::PopStyleColor();

        if (ImGui::IsItemClicked()) {
            ImGuiIO& io = ImGui::GetIO();
            if (io.KeyCtrl) {
                // Ctrl+Click: toggle individual entity in selection
                ToggleEntitySelection(entityID);
            } else if (io.KeyShift) {
                // Shift+Click: range select from last clicked to this entity
                ShiftSelectRange(entityID);
            } else {
                // Plain click: single select (clear others)
                SelectEntityOnly(VE::Entity(entityID, &*m_Scene));
            }
        }

        // Right-click context menu on entity node
        if (ImGui::BeginPopupContextItem()) {
            if (rel.Parent != entt::null) {
                if (ImGui::MenuItem("Unparent")) {
                    m_CommandHistory.Execute("Unparent Entity", [this, entityID]() {
                        m_Scene->RemoveParent(entityID);
                    });
                }
                ImGui::Separator();
            }
            bool multiSelected = m_SelectedEntities.size() > 1 && IsEntitySelected(entityID);
            if (multiSelected) {
                std::string label = "Delete " + std::to_string(m_SelectedEntities.size()) + " Entities";
                if (ImGui::MenuItem(label.c_str())) {
                    DeleteSelectedEntities();
                }
            } else {
                if (ImGui::MenuItem("Delete")) {
                    m_CommandHistory.Execute("Delete Entity", [this, entityID]() {
                        m_Scene->DestroyEntity(VE::Entity(entityID, &*m_Scene));
                        m_SelectedEntities.erase(entityID);
                        if (m_SelectedEntity.GetHandle() == entityID)
                            m_SelectedEntity = {};
                    });
                }
            }
            ImGui::EndPopup();
        }

        // Drag source
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("ENTITY_NODE", &entityID, sizeof(entt::entity));
            ImGui::Text("%s", tag.Tag.c_str());
            ImGui::EndDragDropSource();
        }

        // Drop target: reparent dragged entity under this one
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENTITY_NODE")) {
                entt::entity dropped = *static_cast<const entt::entity*>(payload->Data);
                if (dropped != entityID) {
                    m_CommandHistory.Execute("Reparent Entity", [this, dropped, entityID]() {
                        m_Scene->SetParent(dropped, entityID);
                    });
                }
            }
            ImGui::EndDragDropTarget();
        }

        if (opened) {
            for (auto child : rel.Children)
                DrawEntityNode(child);
            ImGui::TreePop();
        }
    }

    // ── Inspector Panel ────────────────────────────────────────────────

    // ── Asset Inspector ──────────────────────────────────────────────

    void DrawAssetInspector() {
        auto* meta = m_AssetDatabase.GetMetaByPath(m_SelectedAssetPath);
        if (!meta) {
            ImGui::TextDisabled("Asset not found");
            return;
        }

        std::string filename = std::filesystem::path(m_SelectedAssetPath).filename().generic_string();
        ImGui::Text("Asset: %s", filename.c_str());
        ImGui::TextDisabled("Path: Assets/%s", m_SelectedAssetPath.c_str());
        ImGui::TextDisabled("UUID: %llu", static_cast<unsigned long long>(static_cast<uint64_t>(meta->Uuid)));
        ImGui::Separator();

        if (meta->Type == VE::AssetType::MaterialAsset) {
            DrawMaterialAssetInspector();
        } else if (meta->Type == VE::AssetType::Texture2D) {
            std::string absPath = m_AssetDatabase.GetAbsolutePath(m_SelectedAssetPath);
            uint64_t texID = m_ThumbnailCache.GetThumbnail(absPath);
            if (texID != 0)
                ImGui::Image((ImTextureID)texID, ImVec2(128, 128));
            ImGui::Text("Type: Texture2D");
        } else if (meta->Type == VE::AssetType::Shader) {
            ImGui::Text("Type: Shader (.shader)");
        } else if (meta->Type == VE::AssetType::Mesh) {
            DrawMeshAssetInspector();
        } else if (meta->Type == VE::AssetType::Scene) {
            ImGui::Text("Type: Scene");
        } else {
            ImGui::Text("Type: Unknown");
        }
    }

    void DrawMaterialAssetInspector() {
        std::string absPath = m_AssetDatabase.GetAbsolutePath(m_SelectedAssetPath);

        // Load material if not cached
        if (!m_InspectedMaterial || m_InspectedMaterialPath != absPath) {
            m_InspectedMaterial = VE::Material::Load(absPath);
            m_InspectedMaterialPath = absPath;
            if (m_InspectedMaterial)
                VE::MaterialLibrary::Register(m_InspectedMaterial);
        }

        if (!m_InspectedMaterial) {
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "Failed to load material");
            return;
        }

        ImGui::Text("Type: Material");
        if (ImGui::Button("Open in Material Editor")) {
            m_InspectedMaterial->PopulateFromShader();
            m_ShowMaterialEditor = true;
        }
        ImGui::Separator();

        // Name
        char nameBuf[128];
        strncpy(nameBuf, m_InspectedMaterial->GetName().c_str(), sizeof(nameBuf) - 1);
        nameBuf[sizeof(nameBuf) - 1] = '\0';
        if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf)))
            m_InspectedMaterial->SetName(nameBuf);

        // Shader picker (dynamic — lists all registered shaders)
        {
            auto currentShader = m_InspectedMaterial->GetShader();
            std::string currentName = currentShader ? currentShader->GetName() : "None";

            if (ImGui::BeginCombo("Shader", currentName.c_str())) {
                auto allNames = VE::ShaderLibrary::GetAllNames();
                // Sort for stable display order
                std::sort(allNames.begin(), allNames.end());
                for (auto& name : allNames) {
                    auto shader = VE::ShaderLibrary::Get(name);
                    bool isSelected = (shader == currentShader);
                    if (ImGui::Selectable(name.c_str(), isSelected)) {
                        m_InspectedMaterial->SetShader(shader);
                        // Auto-detect lit status: "Lit", "PBR", or "Water" shaders need lighting
                        m_InspectedMaterial->SetLit(name == "Lit" || name == "PBR" || name == "Water");
                        // Auto-populate properties from new shader's ShaderLab declarations
                        m_InspectedMaterial->PopulateFromShader();
                    }
                    if (isSelected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }

        bool isLit = m_InspectedMaterial->IsLit();
        if (ImGui::Checkbox("Is Lit", &isLit))
            m_InspectedMaterial->SetLit(isLit);

        ImGui::Separator();
        ImGui::Text("Properties:");

        // Material properties editor (uses display names and range sliders)
        for (auto& prop : m_InspectedMaterial->GetProperties()) {
            const char* label = prop.DisplayName.empty() ? prop.Name.c_str() : prop.DisplayName.c_str();
            ImGui::PushID(prop.Name.c_str());
            switch (prop.Type) {
                case VE::MaterialPropertyType::Float:
                    if (prop.IsRange)
                        ImGui::SliderFloat(label, &prop.FloatValue, prop.RangeMin, prop.RangeMax, "%.3f");
                    else
                        ImGui::DragFloat(label, &prop.FloatValue, 0.01f);
                    break;
                case VE::MaterialPropertyType::Int:
                    ImGui::DragInt(label, &prop.IntValue);
                    break;
                case VE::MaterialPropertyType::Vec3:
                    ImGui::ColorEdit3(label, &prop.Vec3Value.x);
                    break;
                case VE::MaterialPropertyType::Vec4:
                    ImGui::ColorEdit4(label, &prop.Vec4Value.x);
                    break;
                case VE::MaterialPropertyType::Texture2D: {
                    ImGui::Text("%s: %s", label,
                        prop.TexturePath.empty() ? "(none)" : prop.TexturePath.c_str());
                    if (ImGui::Button("Browse##tex")) {
                        static const char* filter =
                            "Image Files\0*.png;*.jpg;*.jpeg;*.bmp\0All Files\0*.*\0";
                        std::string path = VE::FileDialog::OpenFile(filter, GetWindow().GetNativeWindow());
                        if (!path.empty()) {
                            prop.TexturePath = path;
                            prop.TextureRef = VE::Texture2D::Create(path);
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Clear##tex")) {
                        prop.TexturePath.clear();
                        prop.TextureRef.reset();
                    }
                    break;
                }
            }
            ImGui::PopID();
        }

        ImGui::Separator();

        // Add property
        if (ImGui::Button("Add Property"))
            ImGui::OpenPopup("AddPropPopup");
        if (ImGui::BeginPopup("AddPropPopup")) {
            if (ImGui::MenuItem("Float")) {
                m_InspectedMaterial->SetFloat("u_NewFloat", 0.0f);
            }
            if (ImGui::MenuItem("Color (Vec4)")) {
                m_InspectedMaterial->SetVec4("u_NewColor", glm::vec4(1.0f));
            }
            if (ImGui::MenuItem("Texture")) {
                m_InspectedMaterial->SetTexture("u_Texture", "");
            }
            ImGui::EndPopup();
        }

        ImGui::SameLine();
        if (ImGui::Button("Save Material")) {
            m_InspectedMaterial->Save(absPath);
        }
    }

    // ── Material Editor Panel (dedicated window) ─────────────────────
    void DrawMaterialEditorPanel() {
        if (!m_ShowMaterialEditor) return;
        ImGui::Begin("Material Editor", &m_ShowMaterialEditor);

        // "New Material" button at the top
        if (ImGui::Button("New Material")) {
            CreateAssetFile(m_BrowserCurrentDir, "New Material", ".vmat");
            // Find the newly created file and open it
            m_AssetDatabase.Refresh();
        }

        ImGui::Separator();

        // If no material is loaded, show a hint
        if (!m_InspectedMaterial) {
            ImGui::TextDisabled("No material selected.");
            ImGui::TextDisabled("Double-click a .vmat file in Content Browser,");
            ImGui::TextDisabled("or click 'Edit' on a material in the Inspector.");
            ImGui::End();
            return;
        }

        // ── Header ──────────────────────────────────────────────────
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Material: %s", m_InspectedMaterial->GetName().c_str());

        // Material name
        char nameBuf[128];
        strncpy(nameBuf, m_InspectedMaterial->GetName().c_str(), sizeof(nameBuf) - 1);
        nameBuf[sizeof(nameBuf) - 1] = '\0';
        if (ImGui::InputText("Name##MatEdName", nameBuf, sizeof(nameBuf)))
            m_InspectedMaterial->SetName(nameBuf);

        // File path (read-only)
        if (!m_InspectedMaterialPath.empty()) {
            ImGui::TextDisabled("Path: %s", m_InspectedMaterialPath.c_str());
        }

        ImGui::Spacing();

        // ── Shader Selection ────────────────────────────────────────
        {
            auto currentShader = m_InspectedMaterial->GetShader();
            std::string currentName = currentShader ? currentShader->GetName() : "None";

            ImGui::Text("Shader");
            ImGui::SameLine(100.0f);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::BeginCombo("##MatEdShader", currentName.c_str())) {
                auto allNames = VE::ShaderLibrary::GetAllNames();
                std::sort(allNames.begin(), allNames.end());
                for (auto& name : allNames) {
                    auto shader = VE::ShaderLibrary::Get(name);
                    bool isSelected = (shader == currentShader);
                    if (ImGui::Selectable(name.c_str(), isSelected)) {
                        m_InspectedMaterial->SetShader(shader);
                        // Auto-detect lit status
                        m_InspectedMaterial->SetLit(name == "Lit" || name == "PBR");
                        // Re-populate properties from new shader metadata
                        m_InspectedMaterial->PopulateFromShader();
                    }
                    if (isSelected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }

        // Is Lit checkbox
        bool isLit = m_InspectedMaterial->IsLit();
        if (ImGui::Checkbox("Is Lit##MatEdLit", &isLit))
            m_InspectedMaterial->SetLit(isLit);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ── Properties ──────────────────────────────────────────────
        ImGui::Text("Properties");
        ImGui::Spacing();

        // Get shader property metadata for display hints
        std::unordered_map<std::string, VE::ShaderPropertyInfo> shaderPropMap;
        if (auto shader = m_InspectedMaterial->GetShader()) {
            for (auto& info : shader->GetPropertyInfos())
                shaderPropMap[info.Name] = info;
        }

        bool anyModified = false;
        int removeIdx = -1;
        int propIdx = 0;

        for (auto& prop : m_InspectedMaterial->GetProperties()) {
            ImGui::PushID(propIdx);

            // Use display name from ShaderLab if available, otherwise property name
            const char* label = !prop.DisplayName.empty() ? prop.DisplayName.c_str() : prop.Name.c_str();

            // Determine if this is a Color type from ShaderLab metadata
            bool isColor = false;
            auto it = shaderPropMap.find(prop.Name);
            if (it != shaderPropMap.end()) {
                isColor = (it->second.Type == VE::ShaderPropertyType::Color);
            }

            switch (prop.Type) {
                case VE::MaterialPropertyType::Float: {
                    ImGui::Text("%s", label);
                    ImGui::SameLine(140.0f);
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 30.0f);
                    if (prop.IsRange) {
                        if (ImGui::SliderFloat("##val", &prop.FloatValue, prop.RangeMin, prop.RangeMax, "%.3f"))
                            anyModified = true;
                    } else {
                        if (ImGui::DragFloat("##val", &prop.FloatValue, 0.01f))
                            anyModified = true;
                    }
                    break;
                }
                case VE::MaterialPropertyType::Int: {
                    ImGui::Text("%s", label);
                    ImGui::SameLine(140.0f);
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 30.0f);
                    if (ImGui::DragInt("##val", &prop.IntValue, 1))
                        anyModified = true;
                    break;
                }
                case VE::MaterialPropertyType::Vec3: {
                    ImGui::Text("%s", label);
                    ImGui::SameLine(140.0f);
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 30.0f);
                    if (isColor) {
                        if (ImGui::ColorEdit3("##val", &prop.Vec3Value.x))
                            anyModified = true;
                    } else {
                        if (ImGui::DragFloat3("##val", &prop.Vec3Value.x, 0.01f))
                            anyModified = true;
                    }
                    break;
                }
                case VE::MaterialPropertyType::Vec4: {
                    ImGui::Text("%s", label);
                    ImGui::SameLine(140.0f);
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 30.0f);
                    if (isColor) {
                        if (ImGui::ColorEdit4("##val", &prop.Vec4Value.x))
                            anyModified = true;
                    } else {
                        if (ImGui::DragFloat4("##val", &prop.Vec4Value.x, 0.01f))
                            anyModified = true;
                    }
                    break;
                }
                case VE::MaterialPropertyType::Texture2D: {
                    ImGui::Text("%s", label);
                    ImGui::SameLine(140.0f);

                    // Texture thumbnail preview
                    bool hasTex = prop.TextureRef != nullptr;
                    float thumbSize = 48.0f;

                    ImGui::BeginGroup();
                    if (hasTex) {
                        uint64_t texID = prop.TextureRef->GetNativeTextureID();
                        if (texID != 0) {
                            ImGui::Image((ImTextureID)texID, ImVec2(thumbSize, thumbSize));
                        } else {
                            ImGui::Dummy(ImVec2(thumbSize, thumbSize));
                        }
                    } else {
                        // Empty texture slot placeholder
                        ImVec2 pos = ImGui::GetCursorScreenPos();
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        dl->AddRectFilled(pos, ImVec2(pos.x + thumbSize, pos.y + thumbSize), IM_COL32(60, 60, 60, 255), 3.0f);
                        dl->AddRect(pos, ImVec2(pos.x + thumbSize, pos.y + thumbSize), IM_COL32(100, 100, 100, 255), 3.0f);
                        float cx = pos.x + thumbSize * 0.5f - 10.0f;
                        float cy = pos.y + thumbSize * 0.5f - 6.0f;
                        dl->AddText(ImVec2(cx, cy), IM_COL32(150, 150, 150, 255), "None");
                        ImGui::Dummy(ImVec2(thumbSize, thumbSize));
                    }

                    // Accept texture drag-drop from Content Browser
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_TEXTURE")) {
                            std::string path(static_cast<const char*>(payload->Data));
                            prop.TexturePath = path;
                            prop.TextureRef = VE::Texture2D::Create(path);
                            anyModified = true;
                        }
                        ImGui::EndDragDropTarget();
                    }

                    // Texture path and buttons
                    ImGui::SameLine();
                    ImGui::BeginGroup();
                    ImGui::TextDisabled("%s", prop.TexturePath.empty() ? "(none)" : std::filesystem::path(prop.TexturePath).filename().generic_string().c_str());
                    if (ImGui::SmallButton("Browse##texBrowse")) {
                        static const char* filter =
                            "Image Files\0*.png;*.jpg;*.jpeg;*.bmp\0All Files\0*.*\0";
                        std::string path = VE::FileDialog::OpenFile(filter, GetWindow().GetNativeWindow());
                        if (!path.empty()) {
                            prop.TexturePath = path;
                            prop.TextureRef = VE::Texture2D::Create(path);
                            anyModified = true;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Clear##texClear")) {
                        prop.TexturePath.clear();
                        prop.TextureRef.reset();
                        anyModified = true;
                    }
                    ImGui::EndGroup();
                    ImGui::EndGroup();
                    break;
                }
            }

            // Remove property button
            ImGui::SameLine();
            if (ImGui::SmallButton("X##remove")) {
                removeIdx = propIdx;
            }

            ImGui::PopID();
            propIdx++;
        }

        // Remove property if requested
        if (removeIdx >= 0) {
            auto& props = m_InspectedMaterial->GetProperties();
            if (removeIdx < (int)props.size())
                props.erase(props.begin() + removeIdx);
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ── Add Property ────────────────────────────────────────────
        if (ImGui::Button("Add Property"))
            ImGui::OpenPopup("MatEdAddPropPopup");
        if (ImGui::BeginPopup("MatEdAddPropPopup")) {
            if (ImGui::MenuItem("Float")) {
                m_InspectedMaterial->SetFloat("u_NewFloat", 0.0f);
            }
            if (ImGui::MenuItem("Int")) {
                m_InspectedMaterial->SetInt("u_NewInt", 0);
            }
            if (ImGui::MenuItem("Color (Vec3)")) {
                m_InspectedMaterial->SetVec3("u_NewColor", glm::vec3(1.0f));
            }
            if (ImGui::MenuItem("Color (Vec4)")) {
                m_InspectedMaterial->SetVec4("u_NewColor4", glm::vec4(1.0f));
            }
            if (ImGui::MenuItem("Vector (Vec3)")) {
                m_InspectedMaterial->SetVec3("u_NewVector", glm::vec3(0.0f));
            }
            if (ImGui::MenuItem("Vector (Vec4)")) {
                m_InspectedMaterial->SetVec4("u_NewVector4", glm::vec4(0.0f));
            }
            if (ImGui::MenuItem("Texture")) {
                m_InspectedMaterial->SetTexture("u_NewTexture", "");
            }
            ImGui::EndPopup();
        }

        // ── Populate from Shader ────────────────────────────────────
        ImGui::SameLine();
        if (ImGui::Button("Populate from Shader")) {
            m_InspectedMaterial->PopulateFromShader();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Add any missing properties defined in the shader's Properties {} block.");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ── Save ────────────────────────────────────────────────────
        bool hasPath = !m_InspectedMaterialPath.empty();
        if (hasPath) {
            if (ImGui::Button("Save", ImVec2(120, 0))) {
                m_InspectedMaterial->Save(m_InspectedMaterialPath);
            }
            ImGui::SameLine();
        }
        if (ImGui::Button("Save As...", ImVec2(120, 0))) {
            static const char* filter = "Material Files\0*.vmat\0All Files\0*.*\0";
            std::string path = VE::FileDialog::SaveFile(filter, GetWindow().GetNativeWindow());
            if (!path.empty()) {
                // Ensure .vmat extension
                if (path.size() < 5 || path.substr(path.size() - 5) != ".vmat")
                    path += ".vmat";
                m_InspectedMaterial->Save(path);
                m_InspectedMaterialPath = path;
                VE::MaterialLibrary::Register(m_InspectedMaterial);
                m_AssetDatabase.Refresh();
            }
        }

        ImGui::End();
    }

    void DrawMeshAssetInspector() {
        std::string absPath = m_AssetDatabase.GetAbsolutePath(m_SelectedAssetPath);
        std::string metaPath = absPath + ".meta";

        // Load settings if asset changed
        if (m_InspectedMeshPath != absPath) {
            m_InspectedMeshPath = absPath;
            m_InspectedMeshSettings = VE::FBXImporter::LoadSettings(metaPath);
            m_MeshSettingsDirty = false;

            // If no cached info, do a quick import to populate counts
            if (m_InspectedMeshSettings.VertexCount == 0 || m_InspectedMeshSettings.BoundsSizeX == 0.0f) {
                VE::FBXImportSettings probe = m_InspectedMeshSettings;
                auto mesh = VE::FBXImporter::Import(absPath, probe);
                if (mesh) {
                    m_InspectedMeshSettings.SourceUnitMeters  = probe.SourceUnitMeters;
                    m_InspectedMeshSettings.ImportedUnitMeters = probe.ImportedUnitMeters;
                    m_InspectedMeshSettings.BoundsSizeX       = probe.BoundsSizeX;
                    m_InspectedMeshSettings.BoundsSizeY       = probe.BoundsSizeY;
                    m_InspectedMeshSettings.BoundsSizeZ       = probe.BoundsSizeZ;
                    m_InspectedMeshSettings.VertexCount   = probe.VertexCount;
                    m_InspectedMeshSettings.TriangleCount  = probe.TriangleCount;
                    m_InspectedMeshSettings.SubMeshCount   = probe.SubMeshCount;
                    m_InspectedMeshSettings.BoneCount      = probe.BoneCount;
                    m_InspectedMeshSettings.ClipCount      = probe.ClipCount;
                    VE::FBXImporter::SaveSettings(metaPath, m_InspectedMeshSettings);
                }
            }
        }

        ImGui::Text("Type: Mesh (FBX Importer)");
        ImGui::Separator();

        // ── Mesh Info ──
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Mesh Info");
        ImGui::Text("Vertices:   %u", m_InspectedMeshSettings.VertexCount);
        ImGui::Text("Triangles:  %u", m_InspectedMeshSettings.TriangleCount);
        ImGui::Text("Sub-meshes: %u", m_InspectedMeshSettings.SubMeshCount);
        ImGui::Text("Source Unit: %.6g m", m_InspectedMeshSettings.SourceUnitMeters);
        ImGui::Text("Import Unit: %.6g m", m_InspectedMeshSettings.ImportedUnitMeters);
        ImGui::Text("Bounds: %.3f x %.3f x %.3f",
            m_InspectedMeshSettings.BoundsSizeX,
            m_InspectedMeshSettings.BoundsSizeY,
            m_InspectedMeshSettings.BoundsSizeZ);
        if (m_InspectedMeshSettings.BoneCount > 0)
            ImGui::Text("Bones:      %u", m_InspectedMeshSettings.BoneCount);
        if (m_InspectedMeshSettings.ClipCount > 0)
            ImGui::Text("Anim clips: %u", m_InspectedMeshSettings.ClipCount);
        ImGui::Separator();

        // ── Import Settings ──
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Import Settings");

        if (ImGui::DragFloat("Scale Factor", &m_InspectedMeshSettings.ScaleFactor, 0.01f, 0.001f, 1000.0f))
            m_MeshSettingsDirty = true;

        // Normals combo
        const char* normalItems[] = { "Import", "Calculate", "None" };
        int normalIdx = static_cast<int>(m_InspectedMeshSettings.Normals);
        if (ImGui::Combo("Normals", &normalIdx, normalItems, 3)) {
            m_InspectedMeshSettings.Normals = static_cast<VE::FBXImportSettings::NormalMode>(normalIdx);
            m_MeshSettingsDirty = true;
        }

        if (ImGui::Checkbox("Import UVs", &m_InspectedMeshSettings.ImportUVs))
            m_MeshSettingsDirty = true;
        if (ImGui::Checkbox("Merge All Meshes", &m_InspectedMeshSettings.MergeAllMeshes))
            m_MeshSettingsDirty = true;
        if (ImGui::Checkbox("Import Vertex Colors", &m_InspectedMeshSettings.ImportVertexColors))
            m_MeshSettingsDirty = true;
        if (ImGui::Checkbox("Import Skin Weights", &m_InspectedMeshSettings.ImportSkinWeights))
            m_MeshSettingsDirty = true;
        if (ImGui::Checkbox("Import Animations", &m_InspectedMeshSettings.ImportAnimations))
            m_MeshSettingsDirty = true;

        ImGui::Separator();

        // ── Apply / Revert ──
        bool canApply = m_MeshSettingsDirty;
        if (!canApply) ImGui::BeginDisabled();
        if (ImGui::Button("Apply")) {
            // Re-import with new settings
            VE::FBXImportSettings newSettings = m_InspectedMeshSettings;
            auto mesh = VE::FBXImporter::Import(absPath, newSettings);
            if (mesh) {
                m_InspectedMeshSettings.SourceUnitMeters  = newSettings.SourceUnitMeters;
                m_InspectedMeshSettings.ImportedUnitMeters = newSettings.ImportedUnitMeters;
                m_InspectedMeshSettings.BoundsSizeX       = newSettings.BoundsSizeX;
                m_InspectedMeshSettings.BoundsSizeY       = newSettings.BoundsSizeY;
                m_InspectedMeshSettings.BoundsSizeZ       = newSettings.BoundsSizeZ;
                m_InspectedMeshSettings.VertexCount   = newSettings.VertexCount;
                m_InspectedMeshSettings.TriangleCount  = newSettings.TriangleCount;
                m_InspectedMeshSettings.SubMeshCount   = newSettings.SubMeshCount;
                m_InspectedMeshSettings.BoneCount      = newSettings.BoneCount;
                m_InspectedMeshSettings.ClipCount      = newSettings.ClipCount;

                VE::FBXImporter::SaveSettings(metaPath, m_InspectedMeshSettings);
                VE::MeshImporter::InvalidateCache(absPath);
                auto refreshedMesh = VE::MeshImporter::GetOrLoad(absPath);
                RebindSceneMeshAsset(absPath, refreshedMesh);
                m_MeshSettingsDirty = false;
            }
        }
        if (!canApply) ImGui::EndDisabled();

        ImGui::SameLine();
        if (!canApply) ImGui::BeginDisabled();
        if (ImGui::Button("Revert")) {
            m_InspectedMeshSettings = VE::FBXImporter::LoadSettings(metaPath);
            m_MeshSettingsDirty = false;
        }
        if (!canApply) ImGui::EndDisabled();

        if (m_MeshSettingsDirty) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "(unsaved)");
        }
    }

    // ── Unity-style Object Field for Mesh ──────────────────────────────

    void DrawObjectField(const char* label, VE::MeshRendererComponent& mr) {
        bool isImportedMesh = !mr.MeshSourcePath.empty();

        // Determine display text
        std::string displayName = "None (Mesh)";
        if (isImportedMesh) {
            displayName = std::filesystem::path(mr.MeshSourcePath).stem().generic_string() + " (Mesh)";
        } else {
            for (int i = 0; i < VE::MeshLibrary::GetMeshCount(); i++) {
                if (VE::MeshLibrary::GetMeshByIndex(i) == mr.Mesh) {
                    displayName = std::string(VE::MeshLibrary::GetMeshName(i)) + " (Built-in)";
                    break;
                }
            }
        }

        ImGui::PushID(label);

        // Label on left
        ImGui::Text("%s", label);
        ImGui::SameLine(80.0f);

        // Object field box: [ icon  assetName          (o) ]
        float availW = ImGui::GetContentRegionAvail().x;
        float pickerBtnW = ImGui::GetFrameHeight(); // square button
        float fieldW = availW - pickerBtnW - ImGui::GetStyle().ItemSpacing.x;

        // --- Main field (clickable → ping asset) ---
        {
            ImVec2 pos = ImGui::GetCursorScreenPos();
            ImVec2 size(fieldW, ImGui::GetFrameHeight());

            // Background
            ImU32 bgCol = IM_COL32(40, 40, 40, 255);
            ImU32 borderCol = IM_COL32(80, 80, 80, 255);
            bool hovered = ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + size.x, pos.y + size.y));
            if (hovered) borderCol = IM_COL32(120, 160, 255, 255);

            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bgCol, 3.0f);
            dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), borderCol, 3.0f);

            // Mesh icon indicator
            ImU32 iconCol = isImportedMesh ? IM_COL32(100, 200, 255, 255) : IM_COL32(160, 160, 160, 255);
            float iconSize = size.y * 0.6f;
            float iconPad = (size.y - iconSize) * 0.5f;
            ImVec2 iconMin(pos.x + 4.0f + iconPad, pos.y + iconPad);
            ImVec2 iconMax(iconMin.x + iconSize, iconMin.y + iconSize);
            dl->AddRectFilled(iconMin, iconMax, iconCol, 2.0f);

            // Text
            float textX = iconMax.x + 6.0f;
            float textY = pos.y + (size.y - ImGui::GetTextLineHeight()) * 0.5f;
            dl->AddText(ImVec2(textX, textY), IM_COL32(200, 200, 200, 255), displayName.c_str());

            // Invisible button for interaction
            ImGui::InvisibleButton("##field", size);

            // Click → ping/locate asset in Content Browser
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && isImportedMesh) {
                PingAsset(mr.MeshSourcePath);
            }

            // Accept drag-drop from Content Browser (ASSET_MESH payload)
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_MESH")) {
                    std::string path(static_cast<const char*>(payload->Data));
                    auto meshAsset = VE::MeshImporter::GetOrLoad(path);
                    if (meshAsset && meshAsset->VAO) {
                        mr.Mesh = meshAsset->VAO;
                        mr.MeshSourcePath = path;
                        mr.Mat = VE::MaterialLibrary::Get("Lit");
                    }
                }
                ImGui::EndDragDropTarget();
            }
        }

        // --- Picker button (o) → opens asset picker popup ---
        ImGui::SameLine();
        if (ImGui::Button("o", ImVec2(pickerBtnW, 0)))
            ImGui::OpenPopup("MeshPickerPopup");

        if (ImGui::BeginPopup("MeshPickerPopup")) {
            ImGui::TextDisabled("Built-in");
            // "None" option
            if (ImGui::Selectable("None")) {
                mr.Mesh = nullptr;
                mr.MeshSourcePath.clear();
            }
            for (int i = 0; i < VE::MeshLibrary::GetMeshCount(); i++) {
                if (ImGui::Selectable(VE::MeshLibrary::GetMeshName(i))) {
                    mr.Mesh = VE::MeshLibrary::GetMeshByIndex(i);
                    mr.MeshSourcePath.clear();
                    mr.Mat = VE::MeshLibrary::IsLitMesh(i)
                        ? VE::MaterialLibrary::Get("Lit")
                        : VE::MaterialLibrary::Get("Default");
                }
            }
            ImGui::Separator();
            ImGui::TextDisabled("Assets");
            std::function<void(const std::string&)> listMeshAssets;
            listMeshAssets = [&](const std::string& relDir) {
                for (auto& childPath : m_AssetDatabase.GetDirectoryContents(relDir)) {
                    auto* childMeta = m_AssetDatabase.GetMetaByPath(childPath);
                    if (!childMeta) continue;
                    if (childMeta->Type == VE::AssetType::Folder) {
                        listMeshAssets(childPath);
                    } else if (childMeta->Type == VE::AssetType::Mesh) {
                        std::string absChildPath = m_AssetDatabase.GetAbsolutePath(childPath);
                        std::string itemLabel = std::filesystem::path(childPath).filename().generic_string();
                        if (ImGui::Selectable(itemLabel.c_str())) {
                            auto meshAsset = VE::MeshImporter::GetOrLoad(absChildPath);
                            if (meshAsset && meshAsset->VAO) {
                                mr.Mesh = meshAsset->VAO;
                                mr.MeshSourcePath = absChildPath;
                                mr.Mat = VE::MaterialLibrary::Get("Lit");
                            }
                        }
                    }
                }
            };
            listMeshAssets("");
            ImGui::EndPopup();
        }

        ImGui::PopID();
    }

    // ── Unity-style Object Field for Animation Source ──────────────────

    void DrawAnimationObjectField(const char* label, VE::AnimatorComponent& ac) {
        bool hasSource = !ac.AnimationSourcePath.empty();
        std::string displayName = hasSource
            ? std::filesystem::path(ac.AnimationSourcePath).stem().generic_string() + " (FBX)"
            : "None (Animation)";

        ImGui::PushID(label);

        ImGui::Text("%s", label);
        ImGui::SameLine(80.0f);

        float availW = ImGui::GetContentRegionAvail().x;
        float clearBtnW = ImGui::GetFrameHeight();
        float fieldW = availW - clearBtnW - ImGui::GetStyle().ItemSpacing.x;

        // --- Main field ---
        {
            ImVec2 pos = ImGui::GetCursorScreenPos();
            ImVec2 size(fieldW, ImGui::GetFrameHeight());

            ImU32 bgCol = IM_COL32(40, 40, 40, 255);
            ImU32 borderCol = IM_COL32(80, 80, 80, 255);
            bool hovered = ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + size.x, pos.y + size.y));
            if (hovered) borderCol = IM_COL32(120, 160, 255, 255);

            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bgCol, 3.0f);
            dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), borderCol, 3.0f);

            // Anim icon (orange tint)
            ImU32 iconCol = hasSource ? IM_COL32(255, 180, 80, 255) : IM_COL32(160, 160, 160, 255);
            float iconSize = size.y * 0.6f;
            float iconPad = (size.y - iconSize) * 0.5f;
            ImVec2 iconMin(pos.x + 4.0f + iconPad, pos.y + iconPad);
            ImVec2 iconMax(iconMin.x + iconSize, iconMin.y + iconSize);
            dl->AddRectFilled(iconMin, iconMax, iconCol, 2.0f);

            // Text
            float textX = iconMax.x + 6.0f;
            float textY = pos.y + (size.y - ImGui::GetTextLineHeight()) * 0.5f;
            dl->AddText(ImVec2(textX, textY), IM_COL32(200, 200, 200, 255), displayName.c_str());

            ImGui::InvisibleButton("##animfield", size);

            // Accept drag-drop from Content Browser (ASSET_MESH = FBX files)
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_MESH")) {
                    std::string path(static_cast<const char*>(payload->Data));
                    ac.AnimationSourcePath = path;
                }
                ImGui::EndDragDropTarget();
            }
        }

        // --- Clear button (x) ---
        ImGui::SameLine();
        if (ImGui::Button("x", ImVec2(clearBtnW, 0))) {
            ac.AnimationSourcePath.clear();
            ac.ClipIndex = 0;
        }

        ImGui::PopID();
    }

    // ── Unity-style Object Field for Audio Clip ────────────────────────

    void DrawAudioClipObjectField(const char* label, VE::AudioSourceComponent& as) {
        bool hasClip = !as.ClipPath.empty();
        std::string displayName = hasClip
            ? std::filesystem::path(as.ClipPath).stem().generic_string() + " (AudioClip)"
            : "None (AudioClip)";

        ImGui::PushID(label);

        ImGui::Text("%s", label);
        ImGui::SameLine(80.0f);

        float availW = ImGui::GetContentRegionAvail().x;
        float clearBtnW = ImGui::GetFrameHeight();
        float fieldW = availW - clearBtnW - ImGui::GetStyle().ItemSpacing.x;

        // --- Main field ---
        {
            ImVec2 pos = ImGui::GetCursorScreenPos();
            ImVec2 size(fieldW, ImGui::GetFrameHeight());

            ImU32 bgCol = IM_COL32(40, 40, 40, 255);
            ImU32 borderCol = IM_COL32(80, 80, 80, 255);
            bool hovered = ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + size.x, pos.y + size.y));
            if (hovered) borderCol = IM_COL32(120, 160, 255, 255);

            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bgCol, 3.0f);
            dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), borderCol, 3.0f);

            // Audio icon (green tint)
            ImU32 iconCol = hasClip ? IM_COL32(80, 200, 120, 255) : IM_COL32(160, 160, 160, 255);
            float iconSize = size.y * 0.6f;
            float iconPad = (size.y - iconSize) * 0.5f;
            ImVec2 iconMin(pos.x + 4.0f + iconPad, pos.y + iconPad);
            ImVec2 iconMax(iconMin.x + iconSize, iconMin.y + iconSize);
            dl->AddRectFilled(iconMin, iconMax, iconCol, 2.0f);

            // Text
            float textX = iconMax.x + 6.0f;
            float textY = pos.y + (size.y - ImGui::GetTextLineHeight()) * 0.5f;
            dl->AddText(ImVec2(textX, textY), IM_COL32(200, 200, 200, 255), displayName.c_str());

            ImGui::InvisibleButton("##audiofield", size);

            // Accept drag-drop from Content Browser (ASSET_AUDIO)
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_AUDIO")) {
                    std::string path(static_cast<const char*>(payload->Data));
                    // Stop any preview that was playing with old clip
                    if (as._SoundHandle != 0) {
                        VE::AudioEngine::Stop(as._SoundHandle);
                        as._SoundHandle = 0;
                    }
                    as.ClipPath = path;
                }
                ImGui::EndDragDropTarget();
            }
        }

        // --- Clear button (x) ---
        ImGui::SameLine();
        if (ImGui::Button("x", ImVec2(clearBtnW, 0))) {
            if (as._SoundHandle != 0) {
                VE::AudioEngine::Stop(as._SoundHandle);
                as._SoundHandle = 0;
            }
            as.ClipPath.clear();
        }

        ImGui::PopID();
    }

    void DrawMaterialObjectField(const char* label, VE::MeshRendererComponent& mr) {
        bool hasMat = mr.Mat != nullptr;
        bool isAssetMat = !mr.MaterialPath.empty(); // .vmat file on disk

        // Display text
        std::string displayName = "None (Material)";
        if (hasMat) {
            displayName = mr.Mat->GetName();
            if (isAssetMat)
                displayName += " (Material)";
            else
                displayName += " (Built-in)";
        }

        ImGui::PushID(label);

        ImGui::Text("%s", label);
        ImGui::SameLine(80.0f);

        float availW = ImGui::GetContentRegionAvail().x;
        float pickerBtnW = ImGui::GetFrameHeight();
        float fieldW = availW - pickerBtnW - ImGui::GetStyle().ItemSpacing.x;

        // --- Main field ---
        {
            ImVec2 pos = ImGui::GetCursorScreenPos();
            ImVec2 size(fieldW, ImGui::GetFrameHeight());

            ImU32 bgCol = IM_COL32(40, 40, 40, 255);
            ImU32 borderCol = IM_COL32(80, 80, 80, 255);
            bool hovered = ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + size.x, pos.y + size.y));
            if (hovered) borderCol = IM_COL32(120, 160, 255, 255);

            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bgCol, 3.0f);
            dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), borderCol, 3.0f);

            // Material icon (green sphere-ish circle for materials)
            ImU32 iconCol = hasMat ? IM_COL32(180, 100, 255, 255) : IM_COL32(160, 160, 160, 255);
            float iconSize = size.y * 0.6f;
            float iconPad = (size.y - iconSize) * 0.5f;
            ImVec2 iconCenter(pos.x + 4.0f + iconPad + iconSize * 0.5f, pos.y + size.y * 0.5f);
            dl->AddCircleFilled(iconCenter, iconSize * 0.5f, iconCol);

            // Text
            float textX = pos.x + 4.0f + iconPad + iconSize + 6.0f;
            float textY = pos.y + (size.y - ImGui::GetTextLineHeight()) * 0.5f;
            dl->AddText(ImVec2(textX, textY), IM_COL32(200, 200, 200, 255), displayName.c_str());

            ImGui::InvisibleButton("##matfield", size);

            // Click → ping asset material in Content Browser
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && isAssetMat) {
                PingAsset(mr.MaterialPath);
            }

            // Accept drag-drop from Content Browser
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_MATERIAL")) {
                    std::string path(static_cast<const char*>(payload->Data));
                    auto mat = VE::Material::Load(path);
                    if (mat) {
                        VE::MaterialLibrary::Register(mat);
                        mr.Mat = mat;
                        mr.MaterialPath = path;
                    }
                }
                ImGui::EndDragDropTarget();
            }
        }

        // --- Picker button ---
        ImGui::SameLine();
        if (ImGui::Button("o", ImVec2(pickerBtnW, 0)))
            ImGui::OpenPopup("MaterialPickerPopup");

        if (ImGui::BeginPopup("MaterialPickerPopup")) {
            // "None"
            if (ImGui::Selectable("None")) {
                mr.Mat = nullptr;
                mr.MaterialPath.clear();
            }
            ImGui::Separator();
            ImGui::TextDisabled("Registered");
            auto matNames = VE::MaterialLibrary::GetAllNames();
            std::sort(matNames.begin(), matNames.end());
            for (auto& name : matNames) {
                bool selected = hasMat && mr.Mat->GetName() == name;
                if (ImGui::Selectable(name.c_str(), selected)) {
                    mr.Mat = VE::MaterialLibrary::Get(name);
                    mr.MaterialPath.clear(); // built-in, no file path
                }
            }

            // Asset .vmat files
            ImGui::Separator();
            ImGui::TextDisabled("Assets");
            std::function<void(const std::string&)> listMatAssets;
            listMatAssets = [&](const std::string& relDir) {
                for (auto& childPath : m_AssetDatabase.GetDirectoryContents(relDir)) {
                    auto* childMeta = m_AssetDatabase.GetMetaByPath(childPath);
                    if (!childMeta) continue;
                    if (childMeta->Type == VE::AssetType::Folder) {
                        listMatAssets(childPath);
                    } else if (childMeta->Type == VE::AssetType::MaterialAsset) {
                        std::string absChildPath = m_AssetDatabase.GetAbsolutePath(childPath);
                        std::string itemLabel = std::filesystem::path(childPath).stem().generic_string();
                        bool selected = (isAssetMat && mr.MaterialPath == absChildPath);
                        if (ImGui::Selectable(itemLabel.c_str(), selected)) {
                            auto mat = VE::Material::Load(absChildPath);
                            if (mat) {
                                VE::MaterialLibrary::Register(mat);
                                mr.Mat = mat;
                                mr.MaterialPath = absChildPath;
                            }
                        }
                    }
                }
            };
            listMatAssets("");
            ImGui::EndPopup();
        }

        ImGui::PopID();
    }

    // ── Inspector Panel ────────────────────────────────────────────────

    void DrawInspectorPanel() {
        if (!m_ShowInspector) return;
        ImGui::Begin("Inspector", &m_ShowInspector);

        // Asset inspector (takes priority when an asset is selected)
        if (!m_SelectedAssetPath.empty()) {
            DrawAssetInspector();
            ImGui::End();
            return;
        }

        if (!m_SelectedEntity && m_SelectedEntities.empty()) {
            ImGui::TextDisabled("No selection");
            ImGui::End();
            return;
        }

        // Multi-select header
        if (m_SelectedEntities.size() > 1) {
            ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f),
                "%d entities selected", (int)m_SelectedEntities.size());
            ImGui::Separator();

            // Batch color editing for entities that share MeshRendererComponent
            bool anyHasMesh = false;
            glm::vec4 sharedColor(1.0f);
            bool firstColor = true;
            bool colorsMatch = true;
            for (auto selEntity : m_SelectedEntities) {
                if (!m_Scene->GetRegistry().valid(selEntity)) continue;
                if (!m_Scene->GetRegistry().any_of<VE::MeshRendererComponent>(selEntity)) continue;
                anyHasMesh = true;
                auto& mr = m_Scene->GetRegistry().get<VE::MeshRendererComponent>(selEntity);
                glm::vec4 c(mr.Color[0], mr.Color[1], mr.Color[2], mr.Color[3]);
                if (firstColor) { sharedColor = c; firstColor = false; }
                else if (c != sharedColor) colorsMatch = false;
            }

            if (anyHasMesh) {
                float color[4] = { sharedColor.r, sharedColor.g, sharedColor.b, sharedColor.a };
                if (!colorsMatch)
                    ImGui::TextDisabled("Color: (mixed)");
                if (ImGui::ColorEdit4("Batch Color", color)) {
                    for (auto selEntity : m_SelectedEntities) {
                        if (!m_Scene->GetRegistry().valid(selEntity)) continue;
                        if (!m_Scene->GetRegistry().any_of<VE::MeshRendererComponent>(selEntity)) continue;
                        auto& mr = m_Scene->GetRegistry().get<VE::MeshRendererComponent>(selEntity);
                        mr.Color[0] = color[0]; mr.Color[1] = color[1];
                        mr.Color[2] = color[2]; mr.Color[3] = color[3];
                    }
                }
                if (ImGui::IsItemActivated())
                    m_CommandHistory.BeginPropertyEdit("Batch Color Edit");
                if (ImGui::IsItemDeactivatedAfterEdit())
                    m_CommandHistory.EndPropertyEdit();
            }
            ImGui::Separator();
            ImGui::TextDisabled("Primary entity details below:");
            ImGui::Separator();
        }

        if (!m_SelectedEntity) {
            ImGui::End();
            return;
        }

        if (m_SelectedEntity.HasComponent<VE::TagComponent>()) {
            auto& tag = m_SelectedEntity.GetComponent<VE::TagComponent>();

            // Active toggle + Name on same line
            ImGui::Checkbox("##Active", &tag.Active);
            ImGui::SameLine();
            char buffer[256];
            strncpy(buffer, tag.Tag.c_str(), sizeof(buffer) - 1);
            buffer[sizeof(buffer) - 1] = '\0';
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
            if (ImGui::InputText("##Name", buffer, sizeof(buffer)))
                tag.Tag = buffer;
            if (ImGui::IsItemActivated()) m_CommandHistory.BeginPropertyEdit("Rename Entity");
            if (ImGui::IsItemDeactivatedAfterEdit()) m_CommandHistory.EndPropertyEdit();

            // Tag dropdown
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
            if (ImGui::BeginCombo("Tag", tag.GameObjectTag.c_str())) {
                for (int i = 0; i < VE::kDefaultTagCount; i++) {
                    bool selected = (tag.GameObjectTag == VE::kDefaultTags[i]);
                    if (ImGui::Selectable(VE::kDefaultTags[i], selected)) {
                        auto before = m_CommandHistory.CaptureSnapshot();
                        tag.GameObjectTag = VE::kDefaultTags[i];
                        m_CommandHistory.RecordPropertyEdit("Change Tag", std::move(before));
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            // Layer dropdown
            const char* currentLayerName = (tag.Layer >= 0 && tag.Layer < VE::kLayerCount && VE::kDefaultLayers[tag.Layer][0] != '\0')
                ? VE::kDefaultLayers[tag.Layer] : "Unnamed";
            char layerLabel[64];
            snprintf(layerLabel, sizeof(layerLabel), "%d: %s", tag.Layer, currentLayerName);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
            if (ImGui::BeginCombo("Layer", layerLabel)) {
                for (int i = 0; i < VE::kLayerCount; i++) {
                    const char* lname = VE::kDefaultLayers[i];
                    if (lname[0] == '\0') continue; // skip unnamed layers
                    char itemLabel[64];
                    snprintf(itemLabel, sizeof(itemLabel), "%d: %s", i, lname);
                    bool selected = (tag.Layer == i);
                    if (ImGui::Selectable(itemLabel, selected)) {
                        auto before = m_CommandHistory.CaptureSnapshot();
                        tag.Layer = i;
                        m_CommandHistory.RecordPropertyEdit("Change Layer", std::move(before));
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            ImGui::Separator();
        }

        if (m_SelectedEntity.HasComponent<VE::TransformComponent>()) {
            if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
                auto& tc = m_SelectedEntity.GetComponent<VE::TransformComponent>();
                ImGui::DragFloat3("Position", tc.Position.data(), 0.1f);
                if (ImGui::IsItemActivated()) m_CommandHistory.BeginPropertyEdit("Edit Position");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_CommandHistory.EndPropertyEdit();
                ImGui::DragFloat3("Rotation", tc.Rotation.data(), 1.0f);
                if (ImGui::IsItemActivated()) m_CommandHistory.BeginPropertyEdit("Edit Rotation");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_CommandHistory.EndPropertyEdit();
                ImGui::DragFloat3("Scale",    tc.Scale.data(),    0.1f, 0.01f, 100.0f);
                if (ImGui::IsItemActivated()) m_CommandHistory.BeginPropertyEdit("Edit Scale");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_CommandHistory.EndPropertyEdit();
            }
            ImGui::Separator();
        }

        if (m_SelectedEntity.HasComponent<VE::DirectionalLightComponent>()) {
            bool removeLight = false;
            bool openLight = ImGui::CollapsingHeader("Directional Light", ImGuiTreeNodeFlags_DefaultOpen);
            DrawComponentContextMenu<VE::DirectionalLightComponent>("##DirLightCtx", "DirectionalLight", removeLight);
            if (openLight) {
                auto& dl = m_SelectedEntity.GetComponent<VE::DirectionalLightComponent>();
                glm::vec3 worldDir = m_Scene->GetEntityForward(m_SelectedEntity.GetHandle());
                float direction[3] = { worldDir.x, worldDir.y, worldDir.z };
                ImGui::BeginDisabled();
                ImGui::DragFloat3("World Z Direction", direction, 0.0f);
                ImGui::EndDisabled();
                ImGui::ColorEdit3("Light Color", dl.Color.data());
                if (ImGui::IsItemActivated()) m_CommandHistory.BeginPropertyEdit("Edit Light Color");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_CommandHistory.EndPropertyEdit();
                ImGui::DragFloat("Intensity", &dl.Intensity, 0.01f, 0.0f, 10.0f);
                if (ImGui::IsItemActivated()) m_CommandHistory.BeginPropertyEdit("Edit Light Intensity");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_CommandHistory.EndPropertyEdit();
            }
            if (removeLight)
                m_CommandHistory.Execute("Remove Light", [this]() {
                    m_SelectedEntity.RemoveComponent<VE::DirectionalLightComponent>();
                });
            ImGui::Separator();
        }

        if (m_SelectedEntity.HasComponent<VE::PointLightComponent>()) {
            bool removePointLight = false;
            bool openPointLight = ImGui::CollapsingHeader("Point Light", ImGuiTreeNodeFlags_DefaultOpen);
            DrawComponentContextMenu<VE::PointLightComponent>("##PointLightCtx", "PointLight", removePointLight);
            if (openPointLight) {
                auto& pl = m_SelectedEntity.GetComponent<VE::PointLightComponent>();
                ImGui::ColorEdit3("Color##PL", pl.Color.data());
                if (ImGui::IsItemActivated()) m_CommandHistory.BeginPropertyEdit("Edit Point Light Color");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_CommandHistory.EndPropertyEdit();
                ImGui::DragFloat("Intensity##PL", &pl.Intensity, 0.01f, 0.0f, 100.0f);
                if (ImGui::IsItemActivated()) m_CommandHistory.BeginPropertyEdit("Edit Point Light Intensity");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_CommandHistory.EndPropertyEdit();
                ImGui::DragFloat("Range##PL", &pl.Range, 0.1f, 0.1f, 100.0f);
                if (ImGui::IsItemActivated()) m_CommandHistory.BeginPropertyEdit("Edit Point Light Range");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_CommandHistory.EndPropertyEdit();
            }
            if (removePointLight)
                m_CommandHistory.Execute("Remove Point Light", [this]() {
                    m_SelectedEntity.RemoveComponent<VE::PointLightComponent>();
                });
            ImGui::Separator();
        }

        if (m_SelectedEntity.HasComponent<VE::SpotLightComponent>()) {
            bool removeSpotLight = false;
            bool openSpotLight = ImGui::CollapsingHeader("Spot Light", ImGuiTreeNodeFlags_DefaultOpen);
            DrawComponentContextMenu<VE::SpotLightComponent>("##SpotLightCtx", "SpotLight", removeSpotLight);
            if (openSpotLight) {
                auto& sl = m_SelectedEntity.GetComponent<VE::SpotLightComponent>();
                ImGui::DragFloat3("Direction##SL", sl.Direction.data(), 0.01f);
                if (ImGui::IsItemActivated()) m_CommandHistory.BeginPropertyEdit("Edit Spot Light Direction");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_CommandHistory.EndPropertyEdit();
                ImGui::ColorEdit3("Color##SL", sl.Color.data());
                if (ImGui::IsItemActivated()) m_CommandHistory.BeginPropertyEdit("Edit Spot Light Color");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_CommandHistory.EndPropertyEdit();
                ImGui::DragFloat("Intensity##SL", &sl.Intensity, 0.01f, 0.0f, 100.0f);
                if (ImGui::IsItemActivated()) m_CommandHistory.BeginPropertyEdit("Edit Spot Light Intensity");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_CommandHistory.EndPropertyEdit();
                ImGui::DragFloat("Range##SL", &sl.Range, 0.1f, 0.1f, 200.0f);
                if (ImGui::IsItemActivated()) m_CommandHistory.BeginPropertyEdit("Edit Spot Light Range");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_CommandHistory.EndPropertyEdit();
                ImGui::DragFloat("Inner Angle##SL", &sl.InnerAngle, 0.5f, 0.0f, sl.OuterAngle);
                if (ImGui::IsItemActivated()) m_CommandHistory.BeginPropertyEdit("Edit Spot Light Inner Angle");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_CommandHistory.EndPropertyEdit();
                ImGui::DragFloat("Outer Angle##SL", &sl.OuterAngle, 0.5f, sl.InnerAngle, 89.0f);
                if (ImGui::IsItemActivated()) m_CommandHistory.BeginPropertyEdit("Edit Spot Light Outer Angle");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_CommandHistory.EndPropertyEdit();
            }
            if (removeSpotLight)
                m_CommandHistory.Execute("Remove Spot Light", [this]() {
                    m_SelectedEntity.RemoveComponent<VE::SpotLightComponent>();
                });
            ImGui::Separator();
        }

        if (m_SelectedEntity.HasComponent<VE::AreaLightComponent>()) {
            bool removeAreaLight = false;
            bool openAreaLight = ImGui::CollapsingHeader("Area Light", ImGuiTreeNodeFlags_DefaultOpen);
            DrawComponentContextMenu<VE::AreaLightComponent>("##AreaLightCtx", "AreaLight", removeAreaLight);
            if (openAreaLight) {
                auto& al = m_SelectedEntity.GetComponent<VE::AreaLightComponent>();
                ImGui::ColorEdit3("Color##AL", al.Color.data());
                if (ImGui::IsItemActivated()) m_CommandHistory.BeginPropertyEdit("Edit Area Light Color");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_CommandHistory.EndPropertyEdit();
                ImGui::DragFloat("Intensity##AL", &al.Intensity, 0.01f, 0.0f, 100.0f);
                if (ImGui::IsItemActivated()) m_CommandHistory.BeginPropertyEdit("Edit Area Light Intensity");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_CommandHistory.EndPropertyEdit();
                ImGui::DragFloat("Range##AL", &al.Range, 0.1f, 0.1f, 200.0f);
                if (ImGui::IsItemActivated()) m_CommandHistory.BeginPropertyEdit("Edit Area Light Range");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_CommandHistory.EndPropertyEdit();
                ImGui::DragFloat("Width##AL", &al.Width, 0.05f, 0.05f, 100.0f);
                if (ImGui::IsItemActivated()) m_CommandHistory.BeginPropertyEdit("Edit Area Light Width");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_CommandHistory.EndPropertyEdit();
                ImGui::DragFloat("Height##AL", &al.Height, 0.05f, 0.05f, 100.0f);
                if (ImGui::IsItemActivated()) m_CommandHistory.BeginPropertyEdit("Edit Area Light Height");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_CommandHistory.EndPropertyEdit();
            }
            if (removeAreaLight)
                m_CommandHistory.Execute("Remove Area Light", [this]() {
                    m_SelectedEntity.RemoveComponent<VE::AreaLightComponent>();
                });
            ImGui::Separator();
        }

        if (m_SelectedEntity.HasComponent<VE::ReflectionProbeComponent>()) {
            bool removeRP = false;
            bool openRP = ImGui::CollapsingHeader("Reflection Probe", ImGuiTreeNodeFlags_DefaultOpen);
            DrawComponentContextMenu<VE::ReflectionProbeComponent>("##ReflProbeCtx", "ReflectionProbe", removeRP);
            if (openRP) {
                auto& rp = m_SelectedEntity.GetComponent<VE::ReflectionProbeComponent>();

                // Resolution dropdown
                const int resOptions[] = { 64, 128, 256, 512 };
                const char* resLabels[] = { "64", "128", "256", "512" };
                int currentIdx = 1; // default 128
                for (int i = 0; i < 4; ++i) {
                    if (resOptions[i] == rp.Resolution) { currentIdx = i; break; }
                }
                if (ImGui::Combo("Resolution##RP", &currentIdx, resLabels, 4)) {
                    rp.Resolution = resOptions[currentIdx];
                }

                ImGui::DragFloat3("Box Size##RP", rp.BoxSize.data(), 0.1f, 0.1f, 1000.0f);
                if (ImGui::IsItemActivated()) m_CommandHistory.BeginPropertyEdit("Edit Probe Box Size");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_CommandHistory.EndPropertyEdit();

                ImGui::Checkbox("Bake On Load##RP", &rp.BakeOnLoad);

                // Bake button
                if (ImGui::Button("Bake##RP")) {
                    m_Scene->BakeReflectionProbe(m_SelectedEntity.GetHandle());
                }
                ImGui::SameLine();
                if (ImGui::Button("Bake All Probes##RP")) {
                    m_Scene->BakeReflectionProbes();
                }

                // Status
                if (rp._IsBaked) {
                    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Baked (%dx%d)", rp.Resolution, rp.Resolution);
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "Not Baked");
                }
            }
            if (removeRP)
                m_CommandHistory.Execute("Remove Reflection Probe", [this]() {
                    m_SelectedEntity.RemoveComponent<VE::ReflectionProbeComponent>();
                });
            ImGui::Separator();
        }

        if (m_SelectedEntity.HasComponent<VE::LightProbeComponent>()) {
            bool removeProbe = false;
            bool openProbe = ImGui::CollapsingHeader("Light Probe", ImGuiTreeNodeFlags_DefaultOpen);
            DrawComponentContextMenu<VE::LightProbeComponent>("##LightProbeCtx", "LightProbe", removeProbe);
            if (openProbe) {
                auto& lpc = m_SelectedEntity.GetComponent<VE::LightProbeComponent>();
                ImGui::DragFloat("Radius##LP", &lpc.Radius, 0.5f, 1.0f, 500.0f);
                if (ImGui::IsItemActivated()) m_CommandHistory.BeginPropertyEdit("Edit Probe Radius");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_CommandHistory.EndPropertyEdit();

                const int resOptions[] = { 16, 32, 64, 128 };
                int currentRes = 2; // default 64
                for (int i = 0; i < 4; ++i) if (resOptions[i] == lpc.Resolution) currentRes = i;
                if (ImGui::Combo("Cubemap Resolution##LP", &currentRes, "16\00032\00064\000128\0")) {
                    lpc.Resolution = resOptions[currentRes];
                }

                if (lpc.IsBaked) {
                    ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "BAKED");
                } else {
                    ImGui::TextColored(ImVec4(0.8f, 0.4f, 0.2f, 1.0f), "Not Baked");
                }

                if (ImGui::Button("Bake This Probe")) {
                    auto& tc = m_SelectedEntity.GetComponent<VE::TransformComponent>();
                    glm::vec3 pos(tc.Position[0], tc.Position[1], tc.Position[2]);
                    VE::LightProbe probe;
                    probe.Bake(*m_Scene, pos, static_cast<uint32_t>(lpc.Resolution));
                    auto coeffs = probe.GetCoefficients();
                    for (int i = 0; i < 9; ++i) lpc.SHCoefficients[i] = coeffs[i];
                    lpc.IsBaked = true;
                }
            }
            if (removeProbe)
                m_CommandHistory.Execute("Remove Light Probe", [this]() {
                    m_SelectedEntity.RemoveComponent<VE::LightProbeComponent>();
                });
            ImGui::Separator();
        }

        if (m_SelectedEntity.HasComponent<VE::LightmapComponent>()) {
            bool removeLM = false;
            bool openLM = ImGui::CollapsingHeader("Lightmap", ImGuiTreeNodeFlags_DefaultOpen);
            DrawComponentContextMenu<VE::LightmapComponent>("##LightmapCtx", "Lightmap", removeLM);
            if (openLM) {
                auto& lmc = m_SelectedEntity.GetComponent<VE::LightmapComponent>();
                ImGui::Checkbox("Static##LM", &lmc.IsStatic);

                const int resOptions[] = { 64, 128, 256, 512 };
                int currentRes = 1; // default 128
                for (int i = 0; i < 4; ++i) if (resOptions[i] == lmc.Resolution) currentRes = i;
                if (ImGui::Combo("Resolution##LM", &currentRes, "64\000128\000256\000512\0")) {
                    lmc.Resolution = resOptions[currentRes];
                }

                ImGui::DragFloat("UV Scale X##LM", &lmc.UVScaleX, 0.01f, 0.01f, 10.0f);
                ImGui::DragFloat("UV Scale Y##LM", &lmc.UVScaleY, 0.01f, 0.01f, 10.0f);
                ImGui::DragFloat("UV Offset X##LM", &lmc.UVOffsetX, 0.01f, -10.0f, 10.0f);
                ImGui::DragFloat("UV Offset Y##LM", &lmc.UVOffsetY, 0.01f, -10.0f, 10.0f);

                if (lmc.LightmapTexture) {
                    ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "BAKED (%dx%d)",
                        lmc.LightmapTexture->GetWidth(), lmc.LightmapTexture->GetHeight());
                    // Show lightmap preview
                    ImGui::Image((ImTextureID)lmc.LightmapTexture->GetNativeTextureID(),
                                 ImVec2(128, 128), ImVec2(0, 1), ImVec2(1, 0));
                } else {
                    ImGui::TextColored(ImVec4(0.8f, 0.4f, 0.2f, 1.0f), "Not Baked");
                }
            }
            if (removeLM)
                m_CommandHistory.Execute("Remove Lightmap", [this]() {
                    m_SelectedEntity.RemoveComponent<VE::LightmapComponent>();
                });
            ImGui::Separator();
        }

        if (m_SelectedEntity.HasComponent<VE::RigidbodyComponent>()) {
            bool removeRB = false;
            bool openRB = ImGui::CollapsingHeader("Rigidbody", ImGuiTreeNodeFlags_DefaultOpen);
            DrawComponentContextMenu<VE::RigidbodyComponent>("##RigidbodyCtx", "Rigidbody", removeRB);
            if (openRB) {
                auto& rb = m_SelectedEntity.GetComponent<VE::RigidbodyComponent>();
                const char* bodyTypes[] = { "Static", "Kinematic", "Dynamic" };
                int currentType = static_cast<int>(rb.Type);
                if (ImGui::Combo("Body Type", &currentType, bodyTypes, 3)) {
                    auto before = m_CommandHistory.CaptureSnapshot();
                    rb.Type = static_cast<VE::BodyType>(currentType);
                    m_CommandHistory.RecordPropertyEdit("Change Body Type", std::move(before));
                }
                if (rb.Type == VE::BodyType::Dynamic) {
                    ImGui::DragFloat("Mass", &rb.Mass, 0.1f, 0.01f, 1000.0f);
                    if (ImGui::IsItemActivated()) m_CommandHistory.BeginPropertyEdit("Edit Mass");
                    if (ImGui::IsItemDeactivatedAfterEdit()) m_CommandHistory.EndPropertyEdit();
                }
                ImGui::DragFloat("Linear Damping", &rb.LinearDamping, 0.01f, 0.0f, 10.0f);
                if (ImGui::IsItemActivated()) m_CommandHistory.BeginPropertyEdit("Edit Linear Damping");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_CommandHistory.EndPropertyEdit();
                ImGui::DragFloat("Angular Damping", &rb.AngularDamping, 0.01f, 0.0f, 10.0f);
                if (ImGui::IsItemActivated()) m_CommandHistory.BeginPropertyEdit("Edit Angular Damping");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_CommandHistory.EndPropertyEdit();
                ImGui::DragFloat("Restitution", &rb.Restitution, 0.01f, 0.0f, 1.0f);
                if (ImGui::IsItemActivated()) m_CommandHistory.BeginPropertyEdit("Edit Restitution");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_CommandHistory.EndPropertyEdit();
                ImGui::DragFloat("Friction", &rb.Friction, 0.01f, 0.0f, 2.0f);
                if (ImGui::IsItemActivated()) m_CommandHistory.BeginPropertyEdit("Edit Friction");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_CommandHistory.EndPropertyEdit();
                {
                    bool useGravity = rb.UseGravity;
                    if (ImGui::Checkbox("Use Gravity", &useGravity)) {
                        auto before = m_CommandHistory.CaptureSnapshot();
                        rb.UseGravity = useGravity;
                        m_CommandHistory.RecordPropertyEdit("Toggle Gravity", std::move(before));
                    }
                }
                bool hasAnyCollider = m_SelectedEntity.HasComponent<VE::BoxColliderComponent>()
                    || m_SelectedEntity.HasComponent<VE::SphereColliderComponent>()
                    || m_SelectedEntity.HasComponent<VE::CapsuleColliderComponent>()
                    || m_SelectedEntity.HasComponent<VE::MeshColliderComponent>();
                if (!hasAnyCollider)
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Needs a Collider component to work!");
            }
            if (removeRB)
                m_CommandHistory.Execute("Remove Rigidbody", [this]() {
                    m_SelectedEntity.RemoveComponent<VE::RigidbodyComponent>();
                });
            ImGui::Separator();
        }

        if (m_SelectedEntity.HasComponent<VE::BoxColliderComponent>()) {
            bool remove = false;
            bool openBoxCol = ImGui::CollapsingHeader("Box Collider", ImGuiTreeNodeFlags_DefaultOpen);
            DrawComponentContextMenu<VE::BoxColliderComponent>("##BoxColCtx", "BoxCollider", remove);
            if (openBoxCol) {
                auto& col = m_SelectedEntity.GetComponent<VE::BoxColliderComponent>();
                ImGui::DragFloat3("Size", col.Size.data(), 0.1f, 0.01f, 100.0f);
                if (ImGui::IsItemActivated()) m_CommandHistory.BeginPropertyEdit("Edit Box Size");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_CommandHistory.EndPropertyEdit();
                ImGui::DragFloat3("Offset##Box", col.Offset.data(), 0.1f);
                if (ImGui::IsItemActivated()) m_CommandHistory.BeginPropertyEdit("Edit Box Offset");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_CommandHistory.EndPropertyEdit();
            }
            if (remove) m_CommandHistory.Execute("Remove Box Collider", [this]() { m_SelectedEntity.RemoveComponent<VE::BoxColliderComponent>(); });
            ImGui::Separator();
        }

        if (m_SelectedEntity.HasComponent<VE::SphereColliderComponent>()) {
            bool remove = false;
            bool openSphereCol = ImGui::CollapsingHeader("Sphere Collider", ImGuiTreeNodeFlags_DefaultOpen);
            DrawComponentContextMenu<VE::SphereColliderComponent>("##SphereColCtx", "SphereCollider", remove);
            if (openSphereCol) {
                auto& col = m_SelectedEntity.GetComponent<VE::SphereColliderComponent>();
                ImGui::DragFloat("Radius", &col.Radius, 0.05f, 0.01f, 100.0f);
                if (ImGui::IsItemActivated()) m_CommandHistory.BeginPropertyEdit("Edit Sphere Radius");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_CommandHistory.EndPropertyEdit();
                ImGui::DragFloat3("Offset##Sphere", col.Offset.data(), 0.1f);
                if (ImGui::IsItemActivated()) m_CommandHistory.BeginPropertyEdit("Edit Sphere Offset");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_CommandHistory.EndPropertyEdit();
            }
            if (remove) m_CommandHistory.Execute("Remove Sphere Collider", [this]() { m_SelectedEntity.RemoveComponent<VE::SphereColliderComponent>(); });
            ImGui::Separator();
        }

        if (m_SelectedEntity.HasComponent<VE::CapsuleColliderComponent>()) {
            bool remove = false;
            bool openCapsuleCol = ImGui::CollapsingHeader("Capsule Collider", ImGuiTreeNodeFlags_DefaultOpen);
            DrawComponentContextMenu<VE::CapsuleColliderComponent>("##CapsuleColCtx", "CapsuleCollider", remove);
            if (openCapsuleCol) {
                auto& col = m_SelectedEntity.GetComponent<VE::CapsuleColliderComponent>();
                ImGui::DragFloat("Radius##Cap", &col.Radius, 0.05f, 0.01f, 100.0f);
                if (ImGui::IsItemActivated()) m_CommandHistory.BeginPropertyEdit("Edit Capsule Radius");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_CommandHistory.EndPropertyEdit();
                ImGui::DragFloat("Height##Cap", &col.Height, 0.1f, 0.01f, 100.0f);
                if (ImGui::IsItemActivated()) m_CommandHistory.BeginPropertyEdit("Edit Capsule Height");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_CommandHistory.EndPropertyEdit();
                ImGui::DragFloat3("Offset##Capsule", col.Offset.data(), 0.1f);
                if (ImGui::IsItemActivated()) m_CommandHistory.BeginPropertyEdit("Edit Capsule Offset");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_CommandHistory.EndPropertyEdit();
            }
            if (remove) m_CommandHistory.Execute("Remove Capsule Collider", [this]() { m_SelectedEntity.RemoveComponent<VE::CapsuleColliderComponent>(); });
            ImGui::Separator();
        }

        if (m_SelectedEntity.HasComponent<VE::MeshColliderComponent>()) {
            bool remove = false;
            bool openMeshCol = ImGui::CollapsingHeader("Mesh Collider", ImGuiTreeNodeFlags_DefaultOpen);
            DrawComponentContextMenu<VE::MeshColliderComponent>("##MeshColCtx", "MeshCollider", remove);
            if (openMeshCol) {
                auto& col = m_SelectedEntity.GetComponent<VE::MeshColliderComponent>();
                {
                    bool convex = col.Convex;
                    if (ImGui::Checkbox("Convex", &convex)) {
                        auto before = m_CommandHistory.CaptureSnapshot();
                        col.Convex = convex;
                        m_CommandHistory.RecordPropertyEdit("Toggle Convex", std::move(before));
                    }
                }
                ImGui::DragFloat3("Offset##Mesh", col.Offset.data(), 0.1f);
                if (ImGui::IsItemActivated()) m_CommandHistory.BeginPropertyEdit("Edit Mesh Offset");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_CommandHistory.EndPropertyEdit();
            }
            if (remove) m_CommandHistory.Execute("Remove Mesh Collider", [this]() { m_SelectedEntity.RemoveComponent<VE::MeshColliderComponent>(); });
            ImGui::Separator();
        }

        if (m_SelectedEntity.HasComponent<VE::ScriptComponent>()) {
            auto& sc = m_SelectedEntity.GetComponent<VE::ScriptComponent>();
            bool removeScript = false;
            std::string headerLabel = sc.ClassName.empty() ? "Script" : sc.ClassName;
            bool openScript = ImGui::CollapsingHeader(headerLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
            DrawComponentContextMenu<VE::ScriptComponent>("##ScriptCtx", "Script", removeScript);
            if (openScript) {
                ImGui::Text("Script: %s", sc.ClassName.empty() ? "(none)" : sc.ClassName.c_str());

                // Show properties — from live instance in play mode, or from stored values in edit mode
                if (sc._Instance) {
                    // Play mode: edit instance memory directly, sync back to storage
                    int propCount = 0;
                    auto* props = sc._Instance->GetProperties(propCount);
                    if (propCount > 0) {
                        ImGui::Separator();
                        char* base = reinterpret_cast<char*>(sc._Instance);
                        for (int i = 0; i < propCount; i++) {
                            auto& prop = props[i];
                            void* ptr = base + prop.Offset;
                            switch (prop.Type) {
                                case VE::ScriptPropertyType::Float:
                                    ImGui::DragFloat(prop.Name, static_cast<float*>(ptr), 0.1f);
                                    break;
                                case VE::ScriptPropertyType::Int:
                                    ImGui::DragInt(prop.Name, static_cast<int*>(ptr));
                                    break;
                                case VE::ScriptPropertyType::Bool:
                                    ImGui::Checkbox(prop.Name, static_cast<bool*>(ptr));
                                    break;
                                default: break;
                            }
                        }
                    }
                } else if (!sc.ClassName.empty() && VE::ScriptEngine::IsLoaded()) {
                    // Edit mode: show from cached metadata + stored property values
                    auto cachedProps = VE::ScriptEngine::GetScriptProperties(sc.ClassName);
                    if (!cachedProps.empty()) {
                        ImGui::Separator();
                        for (auto& cp : cachedProps) {
                            auto type = static_cast<VE::ScriptPropertyType>(cp.Type);
                            switch (type) {
                                case VE::ScriptPropertyType::Float: {
                                    auto it = sc.Properties.find(cp.Name);
                                    float val = (it != sc.Properties.end() && std::holds_alternative<float>(it->second))
                                        ? std::get<float>(it->second) : cp.DefaultFloat;
                                    if (ImGui::DragFloat(cp.Name.c_str(), &val, 0.1f))
                                        sc.Properties[cp.Name] = val;
                                    break;
                                }
                                case VE::ScriptPropertyType::Int: {
                                    auto it = sc.Properties.find(cp.Name);
                                    int val = (it != sc.Properties.end() && std::holds_alternative<int>(it->second))
                                        ? std::get<int>(it->second) : cp.DefaultInt;
                                    if (ImGui::DragInt(cp.Name.c_str(), &val))
                                        sc.Properties[cp.Name] = val;
                                    break;
                                }
                                case VE::ScriptPropertyType::Bool: {
                                    auto it = sc.Properties.find(cp.Name);
                                    bool val = (it != sc.Properties.end() && std::holds_alternative<bool>(it->second))
                                        ? std::get<bool>(it->second) : cp.DefaultBool;
                                    if (ImGui::Checkbox(cp.Name.c_str(), &val))
                                        sc.Properties[cp.Name] = val;
                                    break;
                                }
                                default: break;
                            }
                        }
                    }
                }

            }
            if (removeScript)
                m_CommandHistory.Execute("Remove Script", [this]() {
                    m_SelectedEntity.RemoveComponent<VE::ScriptComponent>();
                });
            ImGui::Separator();
        }

        if (m_SelectedEntity.HasComponent<VE::AnimatorComponent>()) {
            auto& ac = m_SelectedEntity.GetComponent<VE::AnimatorComponent>();
            bool removeAnimator = false;
            bool openAnimator = ImGui::CollapsingHeader("Animator", ImGuiTreeNodeFlags_DefaultOpen);
            DrawComponentContextMenu<VE::AnimatorComponent>("##AnimatorCtx", "Animator", removeAnimator);
            if (openAnimator) {
                // Show clip info from the associated MeshAsset
                int clipCount = 0;
                int boneCount = 0;
                if (m_SelectedEntity.HasComponent<VE::MeshRendererComponent>()) {
                    auto& mr = m_SelectedEntity.GetComponent<VE::MeshRendererComponent>();
                    if (!mr.MeshSourcePath.empty()) {
                        auto meshAsset = VE::MeshImporter::GetOrLoad(mr.MeshSourcePath);
                        if (meshAsset) {
                            clipCount = static_cast<int>(meshAsset->Clips.size());
                            if (meshAsset->SkeletonRef)
                                boneCount = meshAsset->SkeletonRef->GetBoneCount();
                        }
                    }
                }
                ImGui::Text("Bones: %d", boneCount);

                // Animation source — Object Field with drag-drop
                DrawAnimationObjectField("Animation", ac);

                ImGui::DragInt("Clip Index", &ac.ClipIndex, 1, 0, 100);

                ImGui::Checkbox("Play On Start", &ac.PlayOnStart);
                ImGui::Checkbox("Loop", &ac.Loop);
                ImGui::DragFloat("Speed", &ac.Speed, 0.05f, 0.0f, 10.0f);

                if (ac._Animator)
                    ImGui::Text("Status: %s", ac._Animator->IsPlaying() ? "Playing" : "Stopped");

                // ── State Machine Editor ─────────────────────────────
                ImGui::Separator();
                ImGui::Checkbox("Use State Machine", &ac.UseStateMachine);
                if (ac.UseStateMachine) {
                    // Parameters
                    if (ImGui::TreeNode("Parameters")) {
                        for (int i = 0; i < (int)ac.Parameters.size(); i++) {
                            ImGui::PushID(i);
                            auto& p = ac.Parameters[i];
                            char nameBuf[128];
                            strncpy(nameBuf, p.Name.c_str(), sizeof(nameBuf)); nameBuf[127] = '\0';
                            ImGui::SetNextItemWidth(100);
                            if (ImGui::InputText("##PName", nameBuf, sizeof(nameBuf))) p.Name = nameBuf;
                            ImGui::SameLine();
                            const char* typeNames[] = { "Float", "Int", "Bool", "Trigger" };
                            ImGui::SetNextItemWidth(70);
                            int typeIdx = static_cast<int>(p.Type);
                            if (ImGui::Combo("##PType", &typeIdx, typeNames, 4))
                                p.Type = static_cast<VE::AnimParamType>(typeIdx);
                            ImGui::SameLine();
                            if (ImGui::SmallButton("X##DelParam")) {
                                ac.Parameters.erase(ac.Parameters.begin() + i); --i;
                            }
                            ImGui::PopID();
                        }
                        if (ImGui::SmallButton("+ Parameter")) {
                            VE::AnimParameter p; p.Name = "Param" + std::to_string(ac.Parameters.size());
                            ac.Parameters.push_back(p);
                        }
                        ImGui::TreePop();
                    }

                    // States
                    if (ImGui::TreeNode("States")) {
                        for (int i = 0; i < (int)ac.States.size(); i++) {
                            ImGui::PushID(i + 1000);
                            auto& s = ac.States[i];
                            char sBuf[128];
                            strncpy(sBuf, s.Name.c_str(), sizeof(sBuf)); sBuf[127] = '\0';
                            ImGui::SetNextItemWidth(100);
                            if (ImGui::InputText("##SName", sBuf, sizeof(sBuf))) s.Name = sBuf;
                            ImGui::SameLine();
                            ImGui::SetNextItemWidth(50);
                            ImGui::DragInt("##SClip", &s.ClipIndex, 1, 0, 100);
                            ImGui::SameLine();
                            ImGui::SetNextItemWidth(50);
                            ImGui::DragFloat("##SSpd", &s.Speed, 0.05f, 0.0f, 10.0f);
                            ImGui::SameLine();
                            ImGui::Checkbox("##SLoop", &s.Loop);
                            ImGui::SameLine();
                            bool isDef = (ac.DefaultState == i);
                            if (isDef) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.3f, 1.0f));
                            if (ImGui::SmallButton(isDef ? "Default" : "Set Default"))
                                ac.DefaultState = i;
                            if (isDef) ImGui::PopStyleColor();
                            ImGui::SameLine();
                            if (ImGui::SmallButton("X##DelState")) {
                                ac.States.erase(ac.States.begin() + i); --i;
                                if (ac.DefaultState >= (int)ac.States.size())
                                    ac.DefaultState = std::max(0, (int)ac.States.size() - 1);
                            }
                            ImGui::PopID();
                        }
                        if (ImGui::SmallButton("+ State")) {
                            VE::AnimState s;
                            s.Name = "State" + std::to_string(ac.States.size());
                            s.ClipIndex = (int)ac.States.size();
                            ac.States.push_back(s);
                        }
                        ImGui::TreePop();
                    }

                    // Transitions
                    if (ImGui::TreeNode("Transitions")) {
                        for (int i = 0; i < (int)ac.Transitions.size(); i++) {
                            ImGui::PushID(i + 2000);
                            auto& t = ac.Transitions[i];

                            // From -> To
                            std::string fromName = (t.FromState < 0) ? "Any" :
                                (t.FromState < (int)ac.States.size() ? ac.States[t.FromState].Name : "?");
                            std::string toName = (t.ToState >= 0 && t.ToState < (int)ac.States.size())
                                ? ac.States[t.ToState].Name : "?";
                            ImGui::Text("%s -> %s", fromName.c_str(), toName.c_str());
                            ImGui::SameLine();
                            ImGui::SetNextItemWidth(40);
                            ImGui::DragInt("##TFrom", &t.FromState, 1, -1, (int)ac.States.size() - 1);
                            ImGui::SameLine();
                            ImGui::SetNextItemWidth(40);
                            ImGui::DragInt("##TTo", &t.ToState, 1, 0, (int)ac.States.size() - 1);

                            ImGui::SetNextItemWidth(60);
                            ImGui::DragFloat("Blend##T", &t.Duration, 0.01f, 0.0f, 2.0f);
                            ImGui::SameLine();
                            ImGui::Checkbox("ExitTime##T", &t.HasExitTime);
                            if (t.HasExitTime) {
                                ImGui::SameLine();
                                ImGui::SetNextItemWidth(50);
                                ImGui::DragFloat("##TExitT", &t.ExitTime, 0.01f, 0.0f, 1.0f);
                            }

                            // Conditions
                            for (int c = 0; c < (int)t.Conditions.size(); c++) {
                                ImGui::PushID(c + 3000);
                                auto& cond = t.Conditions[c];
                                ImGui::Indent(20);
                                char cBuf[64];
                                strncpy(cBuf, cond.ParamName.c_str(), sizeof(cBuf)); cBuf[63] = '\0';
                                ImGui::SetNextItemWidth(80);
                                ImGui::InputText("##CParam", cBuf, sizeof(cBuf));
                                cond.ParamName = cBuf;
                                ImGui::SameLine();
                                const char* opNames[] = { ">", "<", "==", "!=", "true", "false" };
                                int opIdx = static_cast<int>(cond.Op);
                                ImGui::SetNextItemWidth(50);
                                if (ImGui::Combo("##COp", &opIdx, opNames, 6))
                                    cond.Op = static_cast<VE::AnimConditionOp>(opIdx);
                                ImGui::SameLine();
                                ImGui::SetNextItemWidth(50);
                                ImGui::DragFloat("##CVal", &cond.Threshold, 0.1f);
                                ImGui::SameLine();
                                if (ImGui::SmallButton("X##DelCond")) {
                                    t.Conditions.erase(t.Conditions.begin() + c); --c;
                                }
                                ImGui::Unindent(20);
                                ImGui::PopID();
                            }
                            if (ImGui::SmallButton("+ Condition"))
                                t.Conditions.push_back({});

                            ImGui::SameLine();
                            if (ImGui::SmallButton("X##DelTrans")) {
                                ac.Transitions.erase(ac.Transitions.begin() + i); --i;
                            }
                            ImGui::Separator();
                            ImGui::PopID();
                        }
                        if (ImGui::SmallButton("+ Transition"))
                            ac.Transitions.push_back({});
                        ImGui::TreePop();
                    }

                    // Runtime state info
                    if (m_PlayMode && ac._Animator && ac._Animator->IsUsingStateMachine()) {
                        auto& sm = ac._Animator->GetStateMachine();
                        ImGui::Text("Current: %s", sm.GetCurrentStateName().c_str());
                        if (sm.IsTransitioning())
                            ImGui::TextColored(ImVec4(1,1,0,1), "Transitioning...");

                        // Live parameter editing
                        for (auto& p : sm.GetParameters()) {
                            ImGui::PushID(p.Name.c_str());
                            switch (p.Type) {
                                case VE::AnimParamType::Float: {
                                    float v = sm.GetFloat(p.Name);
                                    if (ImGui::DragFloat(p.Name.c_str(), &v, 0.1f))
                                        ac._Animator->GetStateMachine().SetFloat(p.Name, v);
                                    break;
                                }
                                case VE::AnimParamType::Int: {
                                    int v = sm.GetInt(p.Name);
                                    if (ImGui::DragInt(p.Name.c_str(), &v))
                                        ac._Animator->GetStateMachine().SetInt(p.Name, v);
                                    break;
                                }
                                case VE::AnimParamType::Bool: {
                                    bool v = sm.GetBool(p.Name);
                                    if (ImGui::Checkbox(p.Name.c_str(), &v))
                                        ac._Animator->GetStateMachine().SetBool(p.Name, v);
                                    break;
                                }
                                case VE::AnimParamType::Trigger:
                                    if (ImGui::Button(p.Name.c_str()))
                                        ac._Animator->GetStateMachine().SetTrigger(p.Name);
                                    break;
                            }
                            ImGui::PopID();
                        }
                    }
                }

            }
            if (removeAnimator)
                m_SelectedEntity.RemoveComponent<VE::AnimatorComponent>();
            ImGui::Separator();
        }

        // IKComponent inspector
        if (m_SelectedEntity.HasComponent<VE::IKComponent>()) {
            auto& ik = m_SelectedEntity.GetComponent<VE::IKComponent>();
            bool removeIK = false;
            bool openIK = ImGui::CollapsingHeader("Inverse Kinematics", ImGuiTreeNodeFlags_DefaultOpen);
            DrawComponentContextMenu<VE::IKComponent>("##IKCtx", "IK", removeIK);
            if (openIK) {
                // Show bone list from animator if available
                int boneCount = 0;
                VE::Skeleton* skel = nullptr;
                if (m_SelectedEntity.HasComponent<VE::AnimatorComponent>()) {
                    auto& ac = m_SelectedEntity.GetComponent<VE::AnimatorComponent>();
                    if (ac._Animator && ac._Animator->GetMesh() && ac._Animator->GetMesh()->SkeletonRef) {
                        skel = ac._Animator->GetMesh()->SkeletonRef.get();
                        boneCount = skel->GetBoneCount();
                    }
                }

                if (boneCount == 0) {
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                        "Add AnimatorComponent with a skinned mesh to use IK");
                }

                // Add/remove target buttons
                if (ImGui::Button("Add IK Target")) {
                    ik.Targets.push_back({});
                }

                int removeIdx = -1;
                for (int i = 0; i < static_cast<int>(ik.Targets.size()); i++) {
                    ImGui::PushID(i);
                    auto& t = ik.Targets[i];

                    char label[64];
                    snprintf(label, sizeof(label), "Target %d", i);
                    if (ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen)) {
                        ImGui::Checkbox("Enabled", &t.Enabled);

                        // Bone selection — combo box if skeleton available
                        if (skel && boneCount > 0) {
                            const char* boneName = (t.EndBoneIndex >= 0 && t.EndBoneIndex < boneCount)
                                ? skel->Bones[t.EndBoneIndex].Name.c_str() : "None";
                            if (ImGui::BeginCombo("End Bone", boneName)) {
                                for (int b = 0; b < boneCount; b++) {
                                    bool selected = (b == t.EndBoneIndex);
                                    if (ImGui::Selectable(skel->Bones[b].Name.c_str(), selected))
                                        t.EndBoneIndex = b;
                                    if (selected) ImGui::SetItemDefaultFocus();
                                }
                                ImGui::EndCombo();
                            }
                        } else {
                            ImGui::DragInt("End Bone Index", &t.EndBoneIndex, 1.0f, -1, 255);
                        }

                        ImGui::DragInt("Chain Length", &t.ChainLength, 1.0f, 1, 10);
                        ImGui::DragFloat3("Target Position", &t.TargetPosition.x, 0.05f);
                        ImGui::DragFloat3("Pole Vector", &t.PoleVector.x, 0.05f);
                        ImGui::SliderFloat("Weight", &t.Weight, 0.0f, 1.0f);

                        // Target entity UUID (show as drag-int for now)
                        uint64_t uuid = t.TargetEntityUUID;
                        int uuidInt = static_cast<int>(uuid);
                        if (ImGui::DragInt("Target Entity UUID", &uuidInt, 1.0f, 0, 999999)) {
                            t.TargetEntityUUID = static_cast<uint64_t>(uuidInt);
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Clear")) t.TargetEntityUUID = 0;

                        if (ImGui::SmallButton("Remove Target"))
                            removeIdx = i;

                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }

                if (removeIdx >= 0 && removeIdx < static_cast<int>(ik.Targets.size()))
                    ik.Targets.erase(ik.Targets.begin() + removeIdx);
            }
            if (removeIK)
                m_SelectedEntity.RemoveComponent<VE::IKComponent>();
            ImGui::Separator();
        }

        // AudioSourceComponent inspector
        if (m_SelectedEntity.HasComponent<VE::AudioSourceComponent>()) {
            auto& as = m_SelectedEntity.GetComponent<VE::AudioSourceComponent>();
            bool removeAudio = false;
            bool openAudio = ImGui::CollapsingHeader("Audio Source", ImGuiTreeNodeFlags_DefaultOpen);
            DrawComponentContextMenu<VE::AudioSourceComponent>("##AudioSrcCtx", "AudioSource", removeAudio);
            if (openAudio) {
                // ── Audio Clip Object Field (Unity-style) ──
                DrawAudioClipObjectField("Audio Clip", as);

                ImGui::SliderFloat("Volume", &as.Volume, 0.0f, 1.0f);
                ImGui::SliderFloat("Pitch", &as.Pitch, 0.1f, 3.0f);
                ImGui::Checkbox("Loop", &as.Loop);
                ImGui::Checkbox("Play On Awake", &as.PlayOnAwake);
                ImGui::Checkbox("Spatial (3D)", &as.Spatial);
                if (as.Spatial) {
                    ImGui::DragFloat("Min Distance", &as.MinDistance, 0.1f, 0.0f, 1000.0f);
                    ImGui::DragFloat("Max Distance", &as.MaxDistance, 1.0f, 0.0f, 10000.0f);
                }

                // Preview controls (works outside play mode)
                if (!as.ClipPath.empty()) {
                    bool playing = as._SoundHandle != 0 && VE::AudioEngine::IsPlaying(as._SoundHandle);
                    if (playing) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.2f, 0.2f, 1.0f));
                        if (ImGui::Button("Stop Preview")) {
                            VE::AudioEngine::Stop(as._SoundHandle);
                            as._SoundHandle = 0;
                        }
                        ImGui::PopStyleColor();
                    } else {
                        if (ImGui::Button("Play Preview")) {
                            if (as._SoundHandle != 0) {
                                VE::AudioEngine::Stop(as._SoundHandle);
                                as._SoundHandle = 0;
                            }
                            as._SoundHandle = VE::AudioEngine::Play(as.ClipPath, as.Volume, as.Pitch, as.Loop);
                        }
                    }
                }

            }
            if (removeAudio) {
                if (as._SoundHandle != 0) {
                    VE::AudioEngine::Stop(as._SoundHandle);
                }
                m_SelectedEntity.RemoveComponent<VE::AudioSourceComponent>();
            }
            ImGui::Separator();
        }

        // AudioListenerComponent inspector
        if (m_SelectedEntity.HasComponent<VE::AudioListenerComponent>()) {
            bool removeListener = false;
            bool openListener = ImGui::CollapsingHeader("Audio Listener", ImGuiTreeNodeFlags_DefaultOpen);
            DrawComponentContextMenu<VE::AudioListenerComponent>("##AudioListenerCtx", "AudioListener", removeListener);
            if (openListener) {
                ImGui::TextWrapped("This entity acts as the 3D audio listener.");
            }
            if (removeListener)
                m_SelectedEntity.RemoveComponent<VE::AudioListenerComponent>();
            ImGui::Separator();
        }

        // SpriteRendererComponent inspector
        if (m_SelectedEntity.HasComponent<VE::SpriteRendererComponent>()) {
            bool removeSprite = false;
            bool openSprite = ImGui::CollapsingHeader("Sprite Renderer", ImGuiTreeNodeFlags_DefaultOpen);
            DrawComponentContextMenu<VE::SpriteRendererComponent>("##SpriteRendCtx", "SpriteRenderer", removeSprite);
            if (openSprite) {
                auto& sr = m_SelectedEntity.GetComponent<VE::SpriteRendererComponent>();

                ImGui::ColorEdit4("Color##Sprite", sr.Color.data());
                ImGui::DragInt("Sorting Order", &sr.SortingOrder, 1);

                // Texture field
                ImGui::Text("Sprite");
                ImGui::SameLine();
                float fieldW = ImGui::GetContentRegionAvail().x;
                ImVec2 btnSize(fieldW, 20);
                std::string label = sr.TexturePath.empty() ? "None (Texture2D)" : sr.TexturePath;
                ImGui::Button(label.c_str(), btnSize);

                // Drag-drop texture onto field
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_TEXTURE")) {
                        std::string path(static_cast<const char*>(payload->Data));
                        sr.TexturePath = path;
                        sr.Texture = VE::Texture2D::Create(path);
                    }
                    ImGui::EndDragDropTarget();
                }

                if (!sr.TexturePath.empty()) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("X##ClearSpriteTex")) {
                        sr.TexturePath.clear();
                        sr.Texture.reset();
                    }
                }

                // Texture preview
                if (sr.Texture) {
                    ImGui::Image((ImTextureID)sr.Texture->GetNativeTextureID(),
                        ImVec2(64, 64), ImVec2(sr.UVRect[0], sr.UVRect[1] + sr.UVRect[3]),
                        ImVec2(sr.UVRect[0] + sr.UVRect[2], sr.UVRect[1]));
                }

                ImGui::DragFloat4("UV Rect", sr.UVRect.data(), 0.01f, 0.0f, 1.0f);

            }
            if (removeSprite)
                m_SelectedEntity.RemoveComponent<VE::SpriteRendererComponent>();
            ImGui::Separator();
        }

        // SpriteAnimatorComponent inspector
        if (m_SelectedEntity.HasComponent<VE::SpriteAnimatorComponent>()) {
            bool removeSA = false;
            bool openSA = ImGui::CollapsingHeader("Sprite Animator", ImGuiTreeNodeFlags_DefaultOpen);
            DrawComponentContextMenu<VE::SpriteAnimatorComponent>("##SpriteAnimCtx", "SpriteAnimator", removeSA);
            if (openSA) {
                auto& sa = m_SelectedEntity.GetComponent<VE::SpriteAnimatorComponent>();

                int totalFrames = sa.Columns * sa.Rows;
                ImGui::DragInt("Columns", &sa.Columns, 1, 1, 64);
                ImGui::DragInt("Rows", &sa.Rows, 1, 1, 64);
                ImGui::DragInt("Start Frame", &sa.StartFrame, 1, 0, totalFrames - 1);
                ImGui::DragInt("End Frame", &sa.EndFrame, 1, 0, totalFrames - 1);
                ImGui::DragFloat("Frame Rate", &sa.FrameRate, 0.5f, 0.1f, 120.0f);
                ImGui::Checkbox("Loop##SpriteAnim", &sa.Loop);
                ImGui::Checkbox("Play On Start##SpriteAnim", &sa.PlayOnStart);

                if (m_PlayMode)
                    ImGui::Text("Frame: %d / %d", sa._CurrentFrame, sa.EndFrame);

            }
            if (removeSA)
                m_SelectedEntity.RemoveComponent<VE::SpriteAnimatorComponent>();
            ImGui::Separator();
        }

        // ParticleSystemComponent inspector
        if (m_SelectedEntity.HasComponent<VE::ParticleSystemComponent>()) {
            bool removePS = false;
            bool openPS = ImGui::CollapsingHeader("Particle System", ImGuiTreeNodeFlags_DefaultOpen);
            DrawComponentContextMenu<VE::ParticleSystemComponent>("##ParticleCtx", "ParticleSystem", removePS);
            if (openPS) {
                auto& ps = m_SelectedEntity.GetComponent<VE::ParticleSystemComponent>();

                ImGui::DragFloat("Emission Rate", &ps.EmissionRate, 0.5f, 0.0f, 1000.0f);
                ImGui::DragFloat("Lifetime", &ps.ParticleLifetime, 0.1f, 0.01f, 60.0f);
                ImGui::DragFloat("Lifetime Variance", &ps.LifetimeVariance, 0.05f, 0.0f, 30.0f);
                ImGui::DragInt("Max Particles", &ps.MaxParticles, 10, 1, 100000);

                // Emitter shape
                const char* shapeNames[] = { "Point", "Sphere", "Cone" };
                int shapeIdx = static_cast<int>(ps.Shape);
                if (ImGui::Combo("Emitter Shape", &shapeIdx, shapeNames, 3))
                    ps.Shape = static_cast<VE::EmitterShape>(shapeIdx);

                if (ps.Shape == VE::EmitterShape::Point) {
                    ImGui::DragFloat3("Velocity Min", ps.VelocityMin.data(), 0.1f);
                    ImGui::DragFloat3("Velocity Max", ps.VelocityMax.data(), 0.1f);
                } else {
                    ImGui::DragFloat("Speed Min", &ps.SpeedMin, 0.1f, 0.0f, 100.0f);
                    ImGui::DragFloat("Speed Max", &ps.SpeedMax, 0.1f, 0.0f, 100.0f);
                    ImGui::DragFloat("Shape Radius", &ps.ShapeRadius, 0.05f, 0.0f, 50.0f);
                    if (ps.Shape == VE::EmitterShape::Cone)
                        ImGui::DragFloat("Cone Angle", &ps.ConeAngle, 0.5f, 0.0f, 90.0f);
                }

                ImGui::DragFloat3("Gravity", ps.Gravity.data(), 0.1f);
                ImGui::ColorEdit4("Start Color", ps.StartColor.data());
                ImGui::ColorEdit4("End Color", ps.EndColor.data());
                ImGui::DragFloat("Start Size", &ps.StartSize, 0.01f, 0.0f, 10.0f);
                ImGui::DragFloat("End Size", &ps.EndSize, 0.01f, 0.0f, 10.0f);
                ImGui::Checkbox("Looping##Particles", &ps.Looping);
                ImGui::Checkbox("Play On Start##Particles", &ps.PlayOnStart);

                // Texture field with drag-drop
                ImGui::Text("Texture");
                ImGui::SameLine();
                float fieldW = ImGui::GetContentRegionAvail().x;
                std::string texLabel = ps.TexturePath.empty() ? "None (Texture2D)" : ps.TexturePath;
                ImGui::Button(texLabel.c_str(), ImVec2(fieldW, 20));
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_TEXTURE")) {
                        std::string path(static_cast<const char*>(payload->Data));
                        ps.TexturePath = path;
                        ps.Texture = VE::Texture2D::Create(path);
                    }
                    ImGui::EndDragDropTarget();
                }
                if (!ps.TexturePath.empty()) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("X##ClearParticleTex")) {
                        ps.TexturePath.clear();
                        ps.Texture.reset();
                    }
                }

                // Show active particle count in play mode
                if (m_PlayMode && !ps._Particles.empty()) {
                    int active = 0;
                    for (auto& p : ps._Particles)
                        if (p.Active) active++;
                    ImGui::Text("Active Particles: %d / %d", active, ps.MaxParticles);
                }

            }
            if (removePS)
                m_SelectedEntity.RemoveComponent<VE::ParticleSystemComponent>();
            ImGui::Separator();
        }

        // DecalComponent inspector
        if (m_SelectedEntity.HasComponent<VE::DecalComponent>()) {
            bool removeDecal = false;
            bool openDecal = ImGui::CollapsingHeader("Decal", ImGuiTreeNodeFlags_DefaultOpen);
            DrawComponentContextMenu<VE::DecalComponent>("##DecalCtx", "Decal", removeDecal);
            if (openDecal) {
                auto& dc = m_SelectedEntity.GetComponent<VE::DecalComponent>();

                ImGui::ColorEdit4("Color##Decal", dc.Color.data());
                ImGui::DragFloat3("Size##Decal", dc.Size.data(), 0.1f, 0.01f, 100.0f);
                ImGui::SliderFloat("Normal Blend", &dc.NormalBlend, 0.0f, 1.0f);
                ImGui::DragFloat("Fade Distance", &dc.FadeDistance, 0.01f, 0.0f, 1.0f);
                ImGui::DragInt("Sort Order##Decal", &dc.SortOrder, 1);

                // Texture field
                ImGui::Text("Decal Texture");
                ImGui::SameLine();
                float fieldW = ImGui::GetContentRegionAvail().x;
                ImVec2 btnSize(fieldW, 20);
                std::string label = dc.TexturePath.empty() ? "None (Texture2D)" : dc.TexturePath;
                ImGui::Button(label.c_str(), btnSize);

                // Drag-drop texture onto field
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_TEXTURE")) {
                        std::string path(static_cast<const char*>(payload->Data));
                        dc.TexturePath = path;
                        dc._Texture = VE::Texture2D::Create(path);
                    }
                    ImGui::EndDragDropTarget();
                }

                if (!dc.TexturePath.empty()) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("X##ClearDecalTex")) {
                        dc.TexturePath.clear();
                        dc._Texture.reset();
                    }
                }

                // Texture preview
                if (dc._Texture) {
                    ImGui::Image((ImTextureID)dc._Texture->GetNativeTextureID(),
                        ImVec2(64, 64), ImVec2(0, 1), ImVec2(1, 0));
                }
            }
            if (removeDecal)
                m_SelectedEntity.RemoveComponent<VE::DecalComponent>();
            ImGui::Separator();
        }

        // CameraComponent inspector
        if (m_SelectedEntity.HasComponent<VE::CameraComponent>()) {
            bool removeCam = false;
            bool openCam = ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen);
            DrawComponentContextMenu<VE::CameraComponent>("##CameraCtx", "Camera", removeCam);
            if (openCam) {
                auto& cam = m_SelectedEntity.GetComponent<VE::CameraComponent>();

                const char* projTypes[] = { "Perspective", "Orthographic" };
                int currentProj = static_cast<int>(cam.ProjectionType);
                if (ImGui::Combo("Projection", &currentProj, projTypes, 2))
                    cam.ProjectionType = static_cast<VE::CameraProjection>(currentProj);

                if (cam.ProjectionType == VE::CameraProjection::Perspective)
                    ImGui::DragFloat("Field of View", &cam.FOV, 0.5f, 1.0f, 179.0f);
                else
                    ImGui::DragFloat("Size", &cam.Size, 0.1f, 0.01f, 100.0f);

                ImGui::DragFloat("Near Clip", &cam.NearClip, 0.01f, 0.001f, cam.FarClip - 0.001f);
                ImGui::DragFloat("Far Clip",  &cam.FarClip,  1.0f, cam.NearClip + 0.001f, 10000.0f);
                ImGui::DragInt("Priority", &cam.Priority, 1);

            }
            if (removeCam)
                m_SelectedEntity.RemoveComponent<VE::CameraComponent>();
            ImGui::Separator();
        }

        if (m_SelectedEntity.HasComponent<VE::MeshRendererComponent>()) {
            bool removeComponent = false;
            bool openMR = ImGui::CollapsingHeader("Mesh Renderer", ImGuiTreeNodeFlags_DefaultOpen);
            DrawComponentContextMenu<VE::MeshRendererComponent>("##MeshRendererCtx", "MeshRenderer", removeComponent);
            if (openMR) {
                auto& mr = m_SelectedEntity.GetComponent<VE::MeshRendererComponent>();

                // ── Mesh Object Field (Unity-style) ──
                DrawObjectField("Mesh", mr);

                // ── Material Object Field (Unity-style) ──
                DrawMaterialObjectField("Material", mr);

                // "Edit Material" button — opens Material Editor panel
                if (mr.Mat && !mr.MaterialPath.empty()) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Edit##matEdit")) {
                        m_InspectedMaterial = mr.Mat;
                        m_InspectedMaterialPath = mr.MaterialPath;
                        m_InspectedMaterial->PopulateFromShader();
                        m_ShowMaterialEditor = true;
                    }
                }

                ImGui::Checkbox("Cast Shadows", &mr.CastShadows);

                // Per-entity material property overrides
                if (mr.Mat) {
                    // Ensure overrides are synced with material properties (add missing ones)
                    for (const auto& prop : mr.Mat->GetProperties()) {
                        if (prop.Name == "u_EntityColor") continue;
                        bool found = false;
                        for (auto& ov : mr.MaterialOverrides)
                            if (ov.Name == prop.Name) { found = true; break; }
                        if (!found)
                            mr.MaterialOverrides.push_back(prop);
                    }

                    if (!mr.MaterialOverrides.empty()) {
                        ImGui::Separator();
                        ImGui::Text("Material Properties");
                        for (auto& ov : mr.MaterialOverrides) {
                            const char* label = ov.DisplayName.empty() ? ov.Name.c_str() : ov.DisplayName.c_str();
                            ImGui::PushID(ov.Name.c_str());
                            switch (ov.Type) {
                                case VE::MaterialPropertyType::Float:
                                    if (ov.IsRange)
                                        ImGui::SliderFloat(label, &ov.FloatValue, ov.RangeMin, ov.RangeMax, "%.3f");
                                    else
                                        ImGui::DragFloat(label, &ov.FloatValue, 0.01f);
                                    break;
                                case VE::MaterialPropertyType::Int:
                                    ImGui::DragInt(label, &ov.IntValue, 1);
                                    break;
                                case VE::MaterialPropertyType::Vec3:
                                    ImGui::ColorEdit3(label, &ov.Vec3Value.x);
                                    break;
                                case VE::MaterialPropertyType::Vec4:
                                    ImGui::ColorEdit4(label, &ov.Vec4Value.x);
                                    break;
                                case VE::MaterialPropertyType::Texture2D: {
                                    // Texture Object Field (Unity-style drag-drop)
                                    bool hasTex = ov.TextureRef != nullptr;
                                    std::string texDisplay = hasTex
                                        ? std::filesystem::path(ov.TexturePath).filename().generic_string() + " (Texture2D)"
                                        : "None (Texture2D)";

                                    ImGui::Text("%s", label);
                                    ImGui::SameLine(120.0f);

                                    float texAvailW = ImGui::GetContentRegionAvail().x;
                                    float texClearW = ImGui::GetFrameHeight();
                                    float texFieldW = texAvailW - texClearW - ImGui::GetStyle().ItemSpacing.x;

                                    {
                                        ImVec2 pos = ImGui::GetCursorScreenPos();
                                        float fieldH = ImGui::GetFrameHeight();
                                        // If texture is loaded, show thumbnail — make field taller
                                        float thumbSize = hasTex ? 48.0f : 0.0f;
                                        float totalH = hasTex ? std::max(fieldH, thumbSize + 4.0f) : fieldH;
                                        ImVec2 size(texFieldW, totalH);

                                        ImU32 bgCol = IM_COL32(40, 40, 40, 255);
                                        ImU32 borderCol = IM_COL32(80, 80, 80, 255);
                                        bool texHovered = ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + size.x, pos.y + size.y));
                                        if (texHovered) borderCol = IM_COL32(120, 160, 255, 255);

                                        ImDrawList* dl = ImGui::GetWindowDrawList();
                                        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bgCol, 3.0f);
                                        dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), borderCol, 3.0f);

                                        // Thumbnail preview
                                        float textStartX = pos.x + 6.0f;
                                        if (hasTex) {
                                            uint64_t texID = ov.TextureRef->GetNativeTextureID();
                                            if (texID != 0) {
                                                float pad = 2.0f;
                                                ImVec2 thumbMin(pos.x + pad, pos.y + pad);
                                                ImVec2 thumbMax(thumbMin.x + thumbSize, thumbMin.y + thumbSize);
                                                dl->AddImage((ImTextureID)texID, thumbMin, thumbMax);
                                                textStartX = thumbMax.x + 6.0f;
                                            }
                                        }

                                        // Icon (blue square for textures)
                                        if (!hasTex) {
                                            ImU32 iconCol = IM_COL32(100, 160, 220, 255);
                                            float iconSize2 = fieldH * 0.5f;
                                            float iconPad2 = (fieldH - iconSize2) * 0.5f;
                                            ImVec2 iconMin(pos.x + 4.0f + iconPad2, pos.y + iconPad2);
                                            ImVec2 iconMax(iconMin.x + iconSize2, iconMin.y + iconSize2);
                                            dl->AddRectFilled(iconMin, iconMax, iconCol, 2.0f);
                                            textStartX = iconMax.x + 6.0f;
                                        }

                                        // Text
                                        float textY = pos.y + (totalH - ImGui::GetTextLineHeight()) * 0.5f;
                                        dl->AddText(ImVec2(textStartX, textY), IM_COL32(200, 200, 200, 255), texDisplay.c_str());

                                        ImGui::InvisibleButton("##texfield", size);

                                        // Click → file dialog
                                        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                                            static const char* texFilter =
                                                "Image Files (*.png;*.jpg;*.hdr)\0*.png;*.jpg;*.jpeg;*.hdr\0All Files\0*.*\0";
                                            std::string path = VE::FileDialog::OpenFile(texFilter, GetWindow().GetNativeWindow());
                                            if (!path.empty()) {
                                                ov.TexturePath = path;
                                                ov.TextureRef = VE::Texture2D::Create(path);
                                            }
                                        }

                                        // Accept drag-drop from Content Browser (ASSET_TEXTURE)
                                        if (ImGui::BeginDragDropTarget()) {
                                            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_TEXTURE")) {
                                                std::string path(static_cast<const char*>(payload->Data));
                                                ov.TexturePath = path;
                                                ov.TextureRef = VE::Texture2D::Create(path);
                                            }
                                            ImGui::EndDragDropTarget();
                                        }
                                    }

                                    // Clear button (x)
                                    ImGui::SameLine();
                                    if (ImGui::Button("x", ImVec2(texClearW, 0))) {
                                        ov.TexturePath.clear();
                                        ov.TextureRef.reset();
                                    }
                                    break;
                                }
                                default: break;
                            }
                            ImGui::PopID();
                        }
                    }
                }

            }
            if (removeComponent)
                m_CommandHistory.Execute("Remove Mesh Renderer", [this]() {
                    m_SelectedEntity.RemoveComponent<VE::MeshRendererComponent>();
                });
            ImGui::Separator();
        }

        // ── LODGroup Component ──
        if (m_SelectedEntity.HasComponent<VE::LODGroupComponent>()) {
            bool removeLOD = false;
            bool openLOD = ImGui::CollapsingHeader("LOD Group", ImGuiTreeNodeFlags_DefaultOpen);
            DrawComponentContextMenu<VE::LODGroupComponent>("##LODGroupCtx", "LODGroup", removeLOD);
            if (openLOD) {
                auto& lod = m_SelectedEntity.GetComponent<VE::LODGroupComponent>();
                ImGui::DragFloat("Cull Distance", &lod.CullDistance, 1.0f, 0.0f, 10000.0f, "%.0f");
                ImGui::Text("Active LOD: %d / %d", lod._ActiveLOD, static_cast<int>(lod.Levels.size()));

                for (int i = 0; i < static_cast<int>(lod.Levels.size()); ++i) {
                    ImGui::PushID(i);
                    auto& level = lod.Levels[i];

                    // LOD header with color indicator
                    float hue = static_cast<float>(i) / std::max(1, static_cast<int>(lod.Levels.size()));
                    ImVec4 lodColor(1 - hue, hue * 0.7f + 0.3f, 0.3f, 1.0f);
                    if (i == lod._ActiveLOD)
                        lodColor = ImVec4(0.3f, 1.0f, 0.3f, 1.0f);

                    ImGui::TextColored(lodColor, "LOD %d", i);
                    ImGui::SameLine(80);

                    // Mesh info
                    std::string meshName = "None";
                    if (level.MeshType >= 0 && level.MeshType < VE::MeshLibrary::GetMeshCount())
                        meshName = VE::MeshLibrary::GetMeshName(level.MeshType);
                    else if (!level.MeshSourcePath.empty())
                        meshName = std::filesystem::path(level.MeshSourcePath).stem().string();
                    else if (level.Mesh)
                        meshName = "Custom";
                    ImGui::Text("%s", meshName.c_str());
                    ImGui::SameLine(200);

                    ImGui::SetNextItemWidth(100);
                    ImGui::DragFloat("##dist", &level.MaxDistance, 1.0f, 0.0f, 10000.0f, "%.0f m");

                    ImGui::SameLine();
                    if (ImGui::SmallButton("X")) {
                        lod.Levels.erase(lod.Levels.begin() + i);
                        --i;
                    }
                    ImGui::PopID();
                }

                if (ImGui::Button("Add LOD Level")) {
                    VE::LODLevel newLevel;
                    newLevel.MaxDistance = lod.Levels.empty() ? 20.0f
                        : lod.Levels.back().MaxDistance + 20.0f;
                    // Default to Cube mesh
                    newLevel.MeshType = 2;
                    newLevel.Mesh = VE::MeshLibrary::GetCube();
                    lod.Levels.push_back(newLevel);
                }
            }
            if (removeLOD)
                m_SelectedEntity.RemoveComponent<VE::LODGroupComponent>();
            ImGui::Separator();
        }

        // ── Terrain Component Inspector ───────────────────────────
        if (m_SelectedEntity.HasComponent<VE::TerrainComponent>()) {
            bool removeC = false;
            bool openTerrain = ImGui::CollapsingHeader("Terrain", ImGuiTreeNodeFlags_DefaultOpen);
            DrawComponentContextMenu<VE::TerrainComponent>("##TerrainCtx", "Terrain", removeC);
            if (openTerrain) {
                auto& t = m_SelectedEntity.GetComponent<VE::TerrainComponent>();
                bool changed = false;

                // Heightmap source
                char hmBuf[256];
                strncpy(hmBuf, t.HeightmapPath.c_str(), sizeof(hmBuf));
                hmBuf[sizeof(hmBuf)-1] = '\0';
                if (ImGui::InputText("Heightmap Path", hmBuf, sizeof(hmBuf))) {
                    t.HeightmapPath = hmBuf;
                    changed = true;
                }

                changed |= ImGui::DragInt("Resolution", &t.Resolution, 1, 2, 1024);
                changed |= ImGui::DragFloat("World Size X", &t.WorldSizeX, 1.0f, 1.0f, 10000.0f);
                changed |= ImGui::DragFloat("World Size Z", &t.WorldSizeZ, 1.0f, 1.0f, 10000.0f);
                changed |= ImGui::DragFloat("Height Scale", &t.HeightScale, 0.1f, 0.0f, 500.0f);

                if (t.HeightmapPath.empty()) {
                    ImGui::Separator();
                    ImGui::Text("Procedural Noise");
                    changed |= ImGui::DragInt("Octaves", &t.Octaves, 1, 1, 8);
                    changed |= ImGui::DragFloat("Persistence", &t.Persistence, 0.01f, 0.0f, 1.0f);
                    changed |= ImGui::DragFloat("Lacunarity", &t.Lacunarity, 0.1f, 1.0f, 4.0f);
                    changed |= ImGui::DragFloat("Noise Scale", &t.NoiseScale, 1.0f, 1.0f, 500.0f);
                    changed |= ImGui::DragInt("Seed", &t.Seed, 1);
                }

                ImGui::Separator();
                ImGui::Text("Texture Layers");
                const char* layerNames[] = { "Low", "Mid-Low", "Mid-High", "High" };
                for (int i = 0; i < 4; i++) {
                    ImGui::PushID(i);
                    ImGui::Text("Layer %d (%s):", i, layerNames[i]);
                    char texBuf[256];
                    strncpy(texBuf, t.LayerTexturePaths[i].c_str(), sizeof(texBuf));
                    texBuf[sizeof(texBuf)-1] = '\0';
                    if (ImGui::InputText("Texture##Lyr", texBuf, sizeof(texBuf))) {
                        t.LayerTexturePaths[i] = texBuf;
                        if (!t.LayerTexturePaths[i].empty())
                            t._LayerTextures[i] = VE::Texture2D::Create(t.LayerTexturePaths[i]);
                        else
                            t._LayerTextures[i].reset();
                    }
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_TEXTURE")) {
                            t.LayerTexturePaths[i] = std::string(static_cast<const char*>(payload->Data));
                            t._LayerTextures[i] = VE::Texture2D::Create(t.LayerTexturePaths[i]);
                        }
                        ImGui::EndDragDropTarget();
                    }
                    ImGui::DragFloat("Tiling##Lyr", &t.LayerTiling[i], 0.01f, 0.001f, 1.0f);
                    ImGui::PopID();
                }

                ImGui::Separator();
                ImGui::Text("Blend Heights (0-1)");
                ImGui::DragFloat("Low->MidLow", &t.BlendHeights[0], 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("MidLow->MidHigh", &t.BlendHeights[1], 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("MidHigh->High", &t.BlendHeights[2], 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("Roughness##Terrain", &t.Roughness, 0.01f, 0.0f, 1.0f);

                if (changed) t._NeedsRebuild = true;
                if (ImGui::Button("Regenerate"))
                    t._NeedsRebuild = true;

            }
            if (removeC) m_SelectedEntity.RemoveComponent<VE::TerrainComponent>();
            ImGui::Separator();
        }

        // ── UI Components Inspector ──────────────────────────────
        if (m_SelectedEntity.HasComponent<VE::HPWaterComponent>()) {
            bool removeC = false;
            bool openWater = ImGui::CollapsingHeader("HP Water", ImGuiTreeNodeFlags_DefaultOpen);
            DrawComponentContextMenu<VE::HPWaterComponent>("##HPWaterCtx", "HPWater", removeC);
            if (openWater) {
                auto& w = m_SelectedEntity.GetComponent<VE::HPWaterComponent>();
                ImGui::Checkbox("Enabled", &w.Enabled);
                int res = w.Resolution;
                if (ImGui::DragInt("Resolution", &res, 1, 8, 256)) {
                    w.Resolution = std::clamp(res, 8, 256);
                    w._NeedsRebuild = true;
                }
                if (ImGui::DragFloat("World Size X", &w.WorldSizeX, 1.0f, 1.0f, 10000.0f)) w._NeedsRebuild = true;
                if (ImGui::DragFloat("World Size Z", &w.WorldSizeZ, 1.0f, 1.0f, 10000.0f)) w._NeedsRebuild = true;
                if (ImGui::DragFloat("Base Height", &w.BaseHeight, 0.01f, -100.0f, 100.0f)) w._NeedsRebuild = true;
                ImGui::DragFloat("Wave Speed", &w.WaveSpeed, 0.1f, 0.0f, 100.0f);
                ImGui::SliderFloat("Damping", &w.Damping, 0.0f, 0.5f, "%.3f");
                if (ImGui::DragFloat("Height Scale", &w.HeightScale, 0.01f, 0.0f, 10.0f)) w._NeedsRebuild = true;
                ImGui::SliderFloat("Edge Absorption", &w.EdgeAbsorptionWidth, 0.0f, 0.5f, "%.3f");
                ImGui::Checkbox("Spectrum Waves", &w.SpectrumWaves);
                ImGui::DragFloat("Spectrum Amplitude", &w.SpectrumAmplitude, 0.01f, 0.0f, 10.0f, "%.3f");
                ImGui::DragFloat("Spectrum Wind Angle", &w.SpectrumWindAngle, 1.0f, -360.0f, 360.0f, "%.1f");
                ImGui::DragFloat("Spectrum Wind Speed", &w.SpectrumWindSpeed, 0.1f, 0.0f, 80.0f, "%.2f m/s");
                ImGui::SliderFloat("Spectrum Directional Spread", &w.SpectrumDirectionalSpread, 0.0f, 1.0f, "%.3f");
                ImGui::SliderFloat("Spectrum Swell", &w.SpectrumSwell, 0.0f, 1.0f, "%.3f");
                ImGui::SliderFloat("Spectrum Short Wave Fade", &w.SpectrumShortWaveFade, 0.0f, 2.0f, "%.3f");
                ImGui::DragFloat("Spectrum Time Scale", &w.SpectrumTimeScale, 0.01f, 0.0f, 10.0f, "%.3f");
                ImGui::SliderFloat("Spectrum Normal Strength", &w.SpectrumNormalStrength, 0.0f, 4.0f, "%.3f");
                ImGui::SliderFloat("Choppiness", &w.Choppiness, 0.0f, 2.0f, "%.3f");
                ImGui::Checkbox("Auto Impulse", &w.AutoImpulse);
                ImGui::DragFloat("Impulse Interval", &w.AutoImpulseInterval, 0.05f, 0.05f, 20.0f);
                ImGui::DragFloat("Impulse Radius", &w.ImpulseRadius, 0.1f, 0.1f, 100.0f);
                ImGui::DragFloat("Impulse Strength", &w.ImpulseStrength, 0.001f, -1.0f, 1.0f, "%.3f");
                ImGui::ColorEdit3("Scatter", w.ScatterColor.data());
                ImGui::ColorEdit3("Absorption", w.AbsorptionColor.data());
                ImGui::ColorEdit3("Foam", w.FoamColor.data());
                ImGui::SliderFloat("Foam Intensity", &w.FoamIntensity, 0.0f, 2.0f, "%.3f");
                ImGui::SliderFloat("Roughness", &w.Roughness, 0.01f, 0.75f, "%.3f");
                ImGui::SliderFloat("Refraction", &w.RefractionStrength, 0.0f, 1.0f, "%.3f");
                ImGui::SliderFloat("Water Dispersion", &w.WaterDispersionStrength, 0.0f, 2.0f, "%.3f");
                ImGui::DragFloat("Depth Tint Distance", &w.DepthTintDistance, 0.1f, 0.1f, 100.0f);
                int refractionSamples = w.RefractionSampleCount;
                if (ImGui::SliderInt("Refraction Samples", &refractionSamples, 4, 64))
                    w.RefractionSampleCount = std::clamp(refractionSamples, 4, 64);
                ImGui::DragFloat("Max Refraction Cross Distance",
                    &w.MaxRefractionCrossDistance, 0.1f, 0.1f, 200.0f, "%.2f");
                ImGui::DragFloat("Refraction Thickness Offset",
                    &w.RefractionThicknessOffset, 0.01f, 0.01f, 8.0f, "%.2f");
                ImGui::Checkbox("Refraction Jitter", &w.RefractionJitter);
                ImGui::SeparatorText("BSDF");
                ImGui::SliderFloat("Environment Reflection", &w.EnvironmentReflectionIntensity, 0.0f, 3.0f, "%.3f");
                ImGui::SliderFloat("Indirect Light Strength", &w.IndirectLightStrength, 0.0f, 4.0f, "%.3f");
                ImGui::SliderFloat("Macro Scatter", &w.MacroScatterStrength, 0.0f, 4.0f, "%.3f");
                ImGui::SliderFloat("Thin SSS", &w.ThinSSSStrength, 0.0f, 3.0f, "%.3f");
                ImGui::SliderFloat("Backlit Transmission", &w.BacklitTransmissionStrength, 0.0f, 3.0f, "%.3f");
                ImGui::SliderFloat("Forward Scatter", &w.ForwardScatterStrength, 0.0f, 3.0f, "%.3f");
                ImGui::SliderFloat("Forward Scatter Blur Density", &w.ForwardScatterBlurDensity, 0.0f, 4.0f, "%.3f");
                ImGui::SliderFloat("Multi Scatter Scale", &w.MultiScatterScale, 0.0f, 32.0f, "%.2f");
                ImGui::SliderFloat("Phase G", &w.PhaseG, -0.95f, 0.95f, "%.3f");
                ImGui::SliderFloat("Specular FGD", &w.SpecularFGDStrength, 0.0f, 1.0f, "%.3f");
                ImGui::SliderFloat("GGX Energy Compensation", &w.GGXEnergyCompensation, 0.0f, 2.0f, "%.3f");
                ImGui::SeparatorText("Volume Shadows");
                ImGui::SliderFloat("Shadow Softness", &w.VolumeShadowSoftness, 0.0f, 10.0f, "%.2f");
                ImGui::SliderFloat("Min Filter Size", &w.VolumeShadowMinFilterSize, 0.0f, 8.0f, "%.2f");
                int volumeShadowBlockers = w.VolumeShadowBlockerSamples;
                if (ImGui::SliderInt("Blocker Samples", &volumeShadowBlockers, 0, 16))
                    w.VolumeShadowBlockerSamples = std::clamp(volumeShadowBlockers, 0, 16);
                int volumeShadowFilters = w.VolumeShadowFilterSamples;
                if (ImGui::SliderInt("Filter Samples", &volumeShadowFilters, 1, 16))
                    w.VolumeShadowFilterSamples = std::clamp(volumeShadowFilters, 1, 16);
                ImGui::SeparatorText("Volume Temporal / Filter");
                ImGui::SliderFloat("Temporal Blend", &w.VolumeTemporalBlendFactor, 0.0f, 0.98f, "%.3f");
                ImGui::Checkbox("Spatial Filter", &w.VolumeSpatialFilterEnabled);
                int volumeSpatialIterations = w.VolumeSpatialFilterIterations;
                if (ImGui::SliderInt("Spatial Iterations", &volumeSpatialIterations, 1, 3))
                    w.VolumeSpatialFilterIterations = std::clamp(volumeSpatialIterations, 1, 3);
                ImGui::Checkbox("Motion Vectors", &w.VolumeMotionVectorsEnabled);
                ImGui::SliderFloat("Velocity Rejection Scale", &w.VolumeMotionVectorVelocityScale, 0.0f, 10.0f, "%.3f");
                ImGui::Checkbox("Temporal Depth Rejection", &w.VolumeTemporalDepthRejection);
                ImGui::SliderFloat("Temporal Depth Threshold", &w.VolumeTemporalDepthThreshold, 0.0001f, 10.0f, "%.4f");
                ImGui::Checkbox("Spatial Depth Aware", &w.VolumeSpatialDepthAware);
                ImGui::SliderFloat("Spatial Depth Sensitivity", &w.VolumeSpatialDepthSensitivity, 0.0f, 1000.0f, "%.2f");
                ImGui::SeparatorText("Caustics");
                ImGui::Checkbox("Caustics", &w.CausticsEnabled);
                ImGui::SliderFloat("Caustic Strength", &w.CausticStrength, 0.0f, 8.0f, "%.3f");
                ImGui::DragFloat("Caustic Scale", &w.CausticScale, 0.1f, 0.1f, 128.0f, "%.2f");
                ImGui::DragFloat("Caustic Depth Fade", &w.CausticDepthFade, 0.1f, 0.1f, 500.0f, "%.2f");
                ImGui::SliderFloat("Caustic Transmittance", &w.CausticTransmittanceStrength, 0.0f, 8.0f, "%.3f");
                ImGui::SliderFloat("Caustic Leak Reduction", &w.CausticLeakReduction, 0.0f, 1.0f, "%.3f");
                ImGui::SliderFloat("Caustic Shadow Alpha Clip", &w.CausticShadowAlphaClipThreshold, 0.0f, 1.0f, "%.3f");
                ImGui::SliderFloat("Caustic Scatter Boost", &w.CausticScatterBoost, 0.0f, 4.0f, "%.3f");
                ImGui::Checkbox("RGB Caustic", &w.CausticRGBDispersion);
                ImGui::SliderFloat("Caustic Dispersion", &w.CausticDispersionStrength, 0.0f, 2.0f, "%.3f");
                ImGui::Checkbox("Light-Space Atlas", &w.CausticLightSpaceAtlasEnabled);
                int causticAtlasResolution = w.CausticAtlasResolution;
                if (ImGui::DragInt("Caustic Atlas Resolution", &causticAtlasResolution, 16, 128, 2048))
                    w.CausticAtlasResolution = std::clamp(causticAtlasResolution, 128, 2048);
                ImGui::Checkbox("Caustic Filter", &w.CausticFilterEnabled);
                ImGui::DragFloat("Caustic Filter Radius", &w.CausticFilterRadius, 0.05f, 0.25f, 8.0f, "%.2f");
                ImGui::DragFloat("Caustic Depth Sigma", &w.CausticFilterDepthSigma, 0.0001f, 0.00001f, 0.05f, "%.5f");
                ImGui::SliderFloat("Caustic Luminance Weight", &w.CausticFilterLuminanceWeight, 0.0f, 128.0f, "%.2f");
                int causticFilterIterations = w.CausticFilterIterations;
                if (ImGui::SliderInt("Caustic Filter Iterations", &causticFilterIterations, 1, 2))
                    w.CausticFilterIterations = std::clamp(causticFilterIterations, 1, 2);
                ImGui::SliderFloat("Caustic Volume", &w.CausticVolumeStrength, 0.0f, 4.0f, "%.3f");
                ImGui::SeparatorText("Fluid Dynamics");
                ImGui::Checkbox("Fluid Dynamics", &w.FluidDynamicsEnabled);
                int fluidResolution = w.FluidResolution;
                if (ImGui::DragInt("Fluid Resolution", &fluidResolution, 1, 16, 1024))
                    w.FluidResolution = std::clamp(fluidResolution, 16, 1024);
                ImGui::SliderFloat("Fluid Wave Speed", &w.FluidWaveSpeed, 0.0f, 2.0f, "%.3f");
                ImGui::SliderFloat("Fluid Damping", &w.FluidDamping, 0.0f, 0.98f, "%.3f");
                ImGui::DragFloat("Fluid Impulse Radius", &w.FluidImpulseRadius, 0.25f, 1.0f, 128.0f, "%.2f");
                ImGui::DragFloat("Fluid Impulse Strength", &w.FluidImpulseStrength, 0.001f, -1.0f, 1.0f, "%.3f");
                ImGui::Checkbox("Fluid Obstacles", &w.FluidObstaclesEnabled);
                ImGui::Checkbox("Start Frame Bake", &w.FluidStartFrameBake);
                ImGui::DragFloat("Obstacle Padding", &w.FluidObstaclePadding, 0.1f, 0.0f, 20.0f, "%.2f");
                ImGui::DragFloat("Obstacle Height Range", &w.FluidObstacleHeightRange, 0.1f, 0.0f, 50.0f, "%.2f");
            }
            if (removeC) m_SelectedEntity.RemoveComponent<VE::HPWaterComponent>();
            ImGui::Separator();
        }

        if (m_SelectedEntity.HasComponent<VE::UICanvasComponent>()) {
            bool removeC = false;
            bool openC = ImGui::CollapsingHeader("UI Canvas", ImGuiTreeNodeFlags_DefaultOpen);
            DrawComponentContextMenu<VE::UICanvasComponent>("##UICanvasCtx", "UICanvas", removeC);
            if (openC) {
                auto& uc = m_SelectedEntity.GetComponent<VE::UICanvasComponent>();
                ImGui::Checkbox("Screen Space", &uc.ScreenSpace);
                ImGui::DragInt("Sort Order", &uc.SortOrder, 1, 0, 100);
            }
            if (removeC) m_SelectedEntity.RemoveComponent<VE::UICanvasComponent>();
            ImGui::Separator();
        }

        if (m_SelectedEntity.HasComponent<VE::UIRectTransformComponent>()) {
            bool removeC = false;
            bool openC = ImGui::CollapsingHeader("UI Rect Transform", ImGuiTreeNodeFlags_DefaultOpen);
            DrawComponentContextMenu<VE::UIRectTransformComponent>("##UIRectCtx", "UIRectTransform", removeC);
            if (openC) {
                auto& rt = m_SelectedEntity.GetComponent<VE::UIRectTransformComponent>();
                const char* anchorNames[] = {
                    "TopLeft", "TopCenter", "TopRight",
                    "MiddleLeft", "Center", "MiddleRight",
                    "BottomLeft", "BottomCenter", "BottomRight"
                };
                int anchorIdx = static_cast<int>(rt.Anchor);
                if (ImGui::Combo("Anchor", &anchorIdx, anchorNames, 9))
                    rt.Anchor = static_cast<VE::UIAnchorType>(anchorIdx);
                ImGui::DragFloat2("Position", rt.AnchoredPosition.data(), 1.0f);
                ImGui::DragFloat2("Size", rt.Size.data(), 1.0f, 1.0f, 10000.0f);
                ImGui::DragFloat2("Pivot", rt.Pivot.data(), 0.01f, 0.0f, 1.0f);
            }
            if (removeC) m_SelectedEntity.RemoveComponent<VE::UIRectTransformComponent>();
            ImGui::Separator();
        }

        if (m_SelectedEntity.HasComponent<VE::UITextComponent>()) {
            bool removeC = false;
            bool openC = ImGui::CollapsingHeader("UI Text", ImGuiTreeNodeFlags_DefaultOpen);
            DrawComponentContextMenu<VE::UITextComponent>("##UITextCtx", "UIText", removeC);
            if (openC) {
                auto& txt = m_SelectedEntity.GetComponent<VE::UITextComponent>();
                char buf[256];
                strncpy(buf, txt.Text.c_str(), sizeof(buf));
                buf[sizeof(buf)-1] = '\0';
                if (ImGui::InputText("Text", buf, sizeof(buf)))
                    txt.Text = buf;
                ImGui::DragFloat("Font Size", &txt.FontSize, 0.5f, 4.0f, 200.0f);
                ImGui::ColorEdit4("Color##UIText", txt.Color.data());
            }
            if (removeC) m_SelectedEntity.RemoveComponent<VE::UITextComponent>();
            ImGui::Separator();
        }

        if (m_SelectedEntity.HasComponent<VE::UIImageComponent>()) {
            bool removeC = false;
            bool openC = ImGui::CollapsingHeader("UI Image", ImGuiTreeNodeFlags_DefaultOpen);
            DrawComponentContextMenu<VE::UIImageComponent>("##UIImageCtx", "UIImage", removeC);
            if (openC) {
                auto& img = m_SelectedEntity.GetComponent<VE::UIImageComponent>();
                ImGui::ColorEdit4("Color##UIImage", img.Color.data());
                std::string texLabel = img.TexturePath.empty() ? "None" : img.TexturePath;
                ImGui::Text("Texture: %s", texLabel.c_str());
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_TEXTURE")) {
                        img.TexturePath = std::string(static_cast<const char*>(payload->Data));
                        img._Texture = VE::Texture2D::Create(img.TexturePath);
                    }
                    ImGui::EndDragDropTarget();
                }
            }
            if (removeC) m_SelectedEntity.RemoveComponent<VE::UIImageComponent>();
            ImGui::Separator();
        }

        if (m_SelectedEntity.HasComponent<VE::UIButtonComponent>()) {
            bool removeC = false;
            bool openC = ImGui::CollapsingHeader("UI Button", ImGuiTreeNodeFlags_DefaultOpen);
            DrawComponentContextMenu<VE::UIButtonComponent>("##UIButtonCtx", "UIButton", removeC);
            if (openC) {
                auto& btn = m_SelectedEntity.GetComponent<VE::UIButtonComponent>();
                char lblBuf[256];
                strncpy(lblBuf, btn.Label.c_str(), sizeof(lblBuf));
                lblBuf[sizeof(lblBuf)-1] = '\0';
                if (ImGui::InputText("Label##UIBtn", lblBuf, sizeof(lblBuf)))
                    btn.Label = lblBuf;
                ImGui::DragFloat("Font Size##UIBtn", &btn.FontSize, 0.5f, 4.0f, 200.0f);
                ImGui::ColorEdit4("Label Color##UIBtn", btn.LabelColor.data());
                ImGui::ColorEdit4("Normal##UIBtn", btn.NormalColor.data());
                ImGui::ColorEdit4("Hover##UIBtn", btn.HoverColor.data());
                ImGui::ColorEdit4("Pressed##UIBtn", btn.PressedColor.data());
                if (m_PlayMode) {
                    ImGui::Text("Hovered: %s", btn._Hovered ? "Yes" : "No");
                    ImGui::Text("Clicked: %s", btn._Clicked ? "Yes" : "No");
                }
            }
            if (removeC) m_SelectedEntity.RemoveComponent<VE::UIButtonComponent>();
            ImGui::Separator();
        }

        if (ImGui::Button("Add Component", ImVec2(-1, 0))) {
            ImGui::OpenPopup("AddComponentPopup");
            m_AddComponentSearch[0] = '\0';
            m_AddComponentFocusSearch = true;
        }
        if (ImGui::BeginPopup("AddComponentPopup")) {
            // ── Search bar (Unity-style) ──
            if (m_AddComponentFocusSearch) {
                ImGui::SetKeyboardFocusHere();
                m_AddComponentFocusSearch = false;
            }
            ImGui::InputTextWithHint("##ComponentSearch", "Search...", m_AddComponentSearch, sizeof(m_AddComponentSearch));
            ImGui::Separator();

            // Case-insensitive substring match helper
            auto matchesFilter = [this](const char* name) -> bool {
                if (m_AddComponentSearch[0] == '\0') return true;
                std::string lower_name(name), lower_filter(m_AddComponentSearch);
                for (auto& c : lower_name)   c = (char)std::tolower((unsigned char)c);
                for (auto& c : lower_filter) c = (char)std::tolower((unsigned char)c);
                return lower_name.find(lower_filter) != std::string::npos;
            };

            bool anyShown = false;

            // Scripts
            if (!m_SelectedEntity.HasComponent<VE::ScriptComponent>()) {
                auto scriptNames = VE::ScriptEngine::IsLoaded()
                    ? VE::ScriptEngine::GetRegisteredClassNames()
                    : VE::ScriptEngine::ScanScriptClassNames();
                for (auto& name : scriptNames) {
                    std::string label = "Script: " + name;
                    if (matchesFilter(label.c_str())) {
                        if (ImGui::MenuItem(label.c_str())) {
                            std::string scriptName = name;
                            m_CommandHistory.Execute("Add Script", [this, scriptName]() {
                                auto& sc = m_SelectedEntity.AddComponent<VE::ScriptComponent>();
                                sc.ClassName = scriptName;
                                auto cachedProps = VE::ScriptEngine::GetScriptProperties(scriptName);
                                for (auto& cp : cachedProps) {
                                    switch (static_cast<VE::ScriptPropertyType>(cp.Type)) {
                                        case VE::ScriptPropertyType::Float:
                                            sc.Properties[cp.Name] = cp.DefaultFloat; break;
                                        case VE::ScriptPropertyType::Int:
                                            sc.Properties[cp.Name] = cp.DefaultInt; break;
                                        case VE::ScriptPropertyType::Bool:
                                            sc.Properties[cp.Name] = cp.DefaultBool; break;
                                        default: break;
                                    }
                                }
                            });
                            ImGui::CloseCurrentPopup();
                        }
                        anyShown = true;
                    }
                }
            }

            // Macro to reduce repetition for simple components
            #define ADD_COMPONENT_ITEM(Label, CompType, Body) \
                if (!m_SelectedEntity.HasComponent<CompType>() && matchesFilter(Label)) { \
                    if (ImGui::MenuItem(Label)) { Body; ImGui::CloseCurrentPopup(); } \
                    anyShown = true; \
                }

            ADD_COMPONENT_ITEM("Mesh Renderer", VE::MeshRendererComponent,
                m_CommandHistory.Execute("Add Mesh Renderer", [this]() {
                    auto& mr = m_SelectedEntity.AddComponent<VE::MeshRendererComponent>();
                    mr.Mesh = VE::MeshLibrary::GetCube();
                    mr.Mat = VE::MaterialLibrary::Get("Lit");
                }))

            ADD_COMPONENT_ITEM("LOD Group", VE::LODGroupComponent,
                m_CommandHistory.Execute("Add LOD Group", [this]() {
                    m_SelectedEntity.AddComponent<VE::LODGroupComponent>();
                }))

            ADD_COMPONENT_ITEM("Directional Light", VE::DirectionalLightComponent,
                m_CommandHistory.Execute("Add Light", [this]() {
                    m_SelectedEntity.AddComponent<VE::DirectionalLightComponent>();
                }))

            ADD_COMPONENT_ITEM("Point Light", VE::PointLightComponent,
                m_CommandHistory.Execute("Add Point Light", [this]() {
                    m_SelectedEntity.AddComponent<VE::PointLightComponent>();
                }))

            ADD_COMPONENT_ITEM("Spot Light", VE::SpotLightComponent,
                m_CommandHistory.Execute("Add Spot Light", [this]() {
                    m_SelectedEntity.AddComponent<VE::SpotLightComponent>();
                }))

            ADD_COMPONENT_ITEM("Area Light", VE::AreaLightComponent,
                m_CommandHistory.Execute("Add Area Light", [this]() {
                    m_SelectedEntity.AddComponent<VE::AreaLightComponent>();
                }))

            ADD_COMPONENT_ITEM("Reflection Probe", VE::ReflectionProbeComponent,
                m_CommandHistory.Execute("Add Reflection Probe", [this]() {
                    m_SelectedEntity.AddComponent<VE::ReflectionProbeComponent>();
                }))

            ADD_COMPONENT_ITEM("Light Probe", VE::LightProbeComponent,
                m_CommandHistory.Execute("Add Light Probe", [this]() {
                    m_SelectedEntity.AddComponent<VE::LightProbeComponent>();
                }))

            ADD_COMPONENT_ITEM("Lightmap", VE::LightmapComponent,
                m_CommandHistory.Execute("Add Lightmap", [this]() {
                    m_SelectedEntity.AddComponent<VE::LightmapComponent>();
                }))

            ADD_COMPONENT_ITEM("Rigidbody", VE::RigidbodyComponent,
                m_CommandHistory.Execute("Add Rigidbody", [this]() {
                    m_SelectedEntity.AddComponent<VE::RigidbodyComponent>();
                    bool hasCol = m_SelectedEntity.HasComponent<VE::BoxColliderComponent>()
                        || m_SelectedEntity.HasComponent<VE::SphereColliderComponent>()
                        || m_SelectedEntity.HasComponent<VE::CapsuleColliderComponent>()
                        || m_SelectedEntity.HasComponent<VE::MeshColliderComponent>();
                    if (!hasCol)
                        m_SelectedEntity.AddComponent<VE::BoxColliderComponent>();
                }))

            ADD_COMPONENT_ITEM("Box Collider", VE::BoxColliderComponent,
                m_CommandHistory.Execute("Add Box Collider", [this]() {
                    m_SelectedEntity.AddComponent<VE::BoxColliderComponent>();
                }))

            ADD_COMPONENT_ITEM("Sphere Collider", VE::SphereColliderComponent,
                m_CommandHistory.Execute("Add Sphere Collider", [this]() {
                    m_SelectedEntity.AddComponent<VE::SphereColliderComponent>();
                }))

            ADD_COMPONENT_ITEM("Capsule Collider", VE::CapsuleColliderComponent,
                m_CommandHistory.Execute("Add Capsule Collider", [this]() {
                    m_SelectedEntity.AddComponent<VE::CapsuleColliderComponent>();
                }))

            ADD_COMPONENT_ITEM("Mesh Collider", VE::MeshColliderComponent,
                m_CommandHistory.Execute("Add Mesh Collider", [this]() {
                    m_SelectedEntity.AddComponent<VE::MeshColliderComponent>();
                }))

            ADD_COMPONENT_ITEM("Animator", VE::AnimatorComponent,
                m_SelectedEntity.AddComponent<VE::AnimatorComponent>())

            ADD_COMPONENT_ITEM("Inverse Kinematics", VE::IKComponent,
                m_SelectedEntity.AddComponent<VE::IKComponent>())

            ADD_COMPONENT_ITEM("Camera", VE::CameraComponent,
                m_SelectedEntity.AddComponent<VE::CameraComponent>())

            ADD_COMPONENT_ITEM("Audio Source", VE::AudioSourceComponent,
                m_SelectedEntity.AddComponent<VE::AudioSourceComponent>())

            ADD_COMPONENT_ITEM("Audio Listener", VE::AudioListenerComponent,
                m_SelectedEntity.AddComponent<VE::AudioListenerComponent>())

            ADD_COMPONENT_ITEM("Sprite Renderer", VE::SpriteRendererComponent,
                m_SelectedEntity.AddComponent<VE::SpriteRendererComponent>())

            ADD_COMPONENT_ITEM("Sprite Animator", VE::SpriteAnimatorComponent,
                m_SelectedEntity.AddComponent<VE::SpriteAnimatorComponent>())

            ADD_COMPONENT_ITEM("Particle System", VE::ParticleSystemComponent,
                m_SelectedEntity.AddComponent<VE::ParticleSystemComponent>())

            ADD_COMPONENT_ITEM("Decal", VE::DecalComponent,
                m_SelectedEntity.AddComponent<VE::DecalComponent>())

            ADD_COMPONENT_ITEM("Terrain", VE::TerrainComponent,
                m_SelectedEntity.AddComponent<VE::TerrainComponent>())

            ADD_COMPONENT_ITEM("HP Water", VE::HPWaterComponent,
                m_CommandHistory.Execute("Add HP Water", [this]() {
                    auto& water = m_SelectedEntity.AddComponent<VE::HPWaterComponent>();
                    water._NeedsRebuild = true;
                    auto& mr = m_SelectedEntity.HasComponent<VE::MeshRendererComponent>()
                        ? m_SelectedEntity.GetComponent<VE::MeshRendererComponent>()
                        : m_SelectedEntity.AddComponent<VE::MeshRendererComponent>();
                    mr.Mat = VE::MaterialLibrary::Get("Water");
                    mr.Color = { 1.0f, 1.0f, 1.0f, 0.82f };
                    mr.CastShadows = false;
                }))

            ADD_COMPONENT_ITEM("Nav Agent", VE::NavAgentComponent,
                m_SelectedEntity.AddComponent<VE::NavAgentComponent>())

            ADD_COMPONENT_ITEM("UI Canvas", VE::UICanvasComponent,
                m_SelectedEntity.AddComponent<VE::UICanvasComponent>())

            ADD_COMPONENT_ITEM("UI Rect Transform", VE::UIRectTransformComponent,
                m_SelectedEntity.AddComponent<VE::UIRectTransformComponent>())

            ADD_COMPONENT_ITEM("UI Text", VE::UITextComponent,
                m_SelectedEntity.AddComponent<VE::UITextComponent>())

            ADD_COMPONENT_ITEM("UI Image", VE::UIImageComponent,
                m_SelectedEntity.AddComponent<VE::UIImageComponent>())

            ADD_COMPONENT_ITEM("UI Button", VE::UIButtonComponent,
                m_SelectedEntity.AddComponent<VE::UIButtonComponent>())

            #undef ADD_COMPONENT_ITEM

            if (!anyShown) {
                ImGui::TextDisabled("No matching components");
            }
            ImGui::EndPopup();
        }
        ImGui::End();
    }

    // ── Render Pipeline Settings Panel ────────────────────────────────

    void DrawScriptingPanel() {
        if (!m_ShowScripting) return;
        ImGui::Begin("Scripting", &m_ShowScripting);

        // ── Script Project ──────────────────────────────────────────
        if (ImGui::CollapsingHeader("Script Project", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Project: %s", VE::ScriptEngine::GetScriptProjectPath().c_str());

            // New Script button + popup
            if (ImGui::Button("New Script..."))
                ImGui::OpenPopup("NewScriptPopup");

            if (ImGui::BeginPopup("NewScriptPopup")) {
                static char newClassName[128] = {};
                ImGui::Text("Class Name:");
                ImGui::InputText("##newscriptname", newClassName, sizeof(newClassName));
                if (ImGui::Button("Create") && newClassName[0] != '\0') {
                    if (VE::ScriptEngine::CreateNewScript(newClassName))
                        VE_INFO("Created script: {0}", newClassName);
                    newClassName[0] = '\0';
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }

            ImGui::SameLine();
            if (ImGui::Button("Regenerate Registry"))
                VE::ScriptEngine::RegenerateScriptRegistry();
        }

        ImGui::Separator();

        // ── Build ───────────────────────────────────────────────────
        if (ImGui::CollapsingHeader("Build", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto buildStatus = VE::ScriptEngine::GetBuildStatus();
            bool building = (buildStatus == VE::ScriptEngine::BuildStatus::Building);

            if (building) {
                ImGui::BeginDisabled();
                ImGui::Button("Building...");
                ImGui::EndDisabled();
            } else {
                if (ImGui::Button("Build Scripts (Ctrl+R)")) {
                    VE::ScriptEngine::RequestBuildAndReload();
                }
            }

            bool autoReload = VE::ScriptEngine::IsAutoReloadEnabled();
            if (ImGui::Checkbox("Auto-build on save", &autoReload))
                VE::ScriptEngine::SetAutoReloadEnabled(autoReload);

            // Status
            switch (buildStatus) {
                case VE::ScriptEngine::BuildStatus::Building:
                    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.2f, 1.0f), "Building...");
                    break;
                case VE::ScriptEngine::BuildStatus::Success:
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "Build Succeeded");
                    break;
                case VE::ScriptEngine::BuildStatus::Failed:
                    ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "Build Failed");
                    break;
                default: break;
            }

            // Build log
            auto& buildOutput = VE::ScriptEngine::GetBuildOutput();
            if (!buildOutput.empty()) {
                if (ImGui::TreeNode("Build Log")) {
                    ImGui::BeginChild("BuildLog", ImVec2(0, 200), true);
                    ImGui::TextUnformatted(buildOutput.c_str());
                    ImGui::EndChild();
                    ImGui::TreePop();
                }
            }
        }

        ImGui::Separator();

        // ── DLL Management ──────────────────────────────────────────
        if (ImGui::CollapsingHeader("DLL", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Script DLL: %s",
                VE::ScriptEngine::IsLoaded()
                    ? VE::ScriptEngine::GetDLLPath().c_str()
                    : "(none)");

            if (ImGui::Button("Load Script DLL...")) {
                static const char* dllFilter = "DLL Files (*.dll)\0*.dll\0All Files\0*.*\0";
                std::string path = VE::FileDialog::OpenFile(dllFilter, GetWindow().GetNativeWindow());
                if (!path.empty()) {
                    VE::ScriptEngine::LoadScriptDLL(path);
                    m_ScriptDLLPath = path;
                }
            }

            if (VE::ScriptEngine::IsLoaded()) {
                ImGui::SameLine();
                if (ImGui::Button("Unload"))
                    VE::ScriptEngine::UnloadScriptDLL();

                ImGui::Separator();
                ImGui::Text("Registered Scripts:");
                auto names = VE::ScriptEngine::GetRegisteredClassNames();
                for (auto& name : names)
                    ImGui::BulletText("%s", name.c_str());

                if (VE::ScriptEngine::WasReloadedThisFrame())
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "Hot-reloaded!");
            }
        }

        ImGui::End();
    }

    // ── Curve Editor Widget ──────────────────────────────────────────
    void DrawCurveEditor(const char* label,
                         std::vector<std::pair<float, float>>& points,
                         ImU32 curveColor) {
        ImGui::PushID(label);
        if (ImGui::TreeNode(label)) {
            const float CANVAS_SIZE = 200.0f;
            ImVec2 canvasPos = ImGui::GetCursorScreenPos();
            ImVec2 canvasSize(CANVAS_SIZE, CANVAS_SIZE);
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // Background
            dl->AddRectFilled(canvasPos,
                ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                IM_COL32(30, 30, 30, 255));
            // Grid lines
            for (int i = 1; i < 4; i++) {
                float t = i / 4.0f;
                dl->AddLine(
                    ImVec2(canvasPos.x + t * canvasSize.x, canvasPos.y),
                    ImVec2(canvasPos.x + t * canvasSize.x, canvasPos.y + canvasSize.y),
                    IM_COL32(60, 60, 60, 255));
                dl->AddLine(
                    ImVec2(canvasPos.x, canvasPos.y + t * canvasSize.y),
                    ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + t * canvasSize.y),
                    IM_COL32(60, 60, 60, 255));
            }
            // Diagonal reference
            dl->AddLine(canvasPos,
                ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                IM_COL32(80, 80, 80, 128));

            // Draw curve (evaluate many points)
            ImVec2 prevPt;
            for (int i = 0; i <= 128; i++) {
                float t = i / 128.0f;
                // Simple piecewise linear eval for drawing
                float val = t;
                for (size_t s = 0; s + 1 < points.size(); s++) {
                    if (t >= points[s].first && t <= points[s + 1].first) {
                        float seg = points[s + 1].first - points[s].first;
                        float lt = (seg > 1e-6f) ? (t - points[s].first) / seg : 0.0f;
                        val = points[s].second + lt * (points[s + 1].second - points[s].second);
                        break;
                    }
                }
                if (!points.empty() && t <= points.front().first) val = points.front().second;
                if (!points.empty() && t >= points.back().first)  val = points.back().second;

                ImVec2 pt(canvasPos.x + t * canvasSize.x,
                          canvasPos.y + (1.0f - val) * canvasSize.y);
                if (i > 0)
                    dl->AddLine(prevPt, pt, curveColor, 2.0f);
                prevPt = pt;
            }

            // Invisible button for interaction
            ImGui::InvisibleButton("canvas", canvasSize);
            bool canvasHovered = ImGui::IsItemHovered();

            // Draw and handle control points
            int dragIdx = -1;
            for (size_t i = 0; i < points.size(); i++) {
                ImVec2 ptScreen(canvasPos.x + points[i].first * canvasSize.x,
                                canvasPos.y + (1.0f - points[i].second) * canvasSize.y);
                float radius = 5.0f;

                ImGui::SetCursorScreenPos(ImVec2(ptScreen.x - radius, ptScreen.y - radius));
                ImGui::PushID(static_cast<int>(i));
                ImGui::InvisibleButton("pt", ImVec2(radius * 2, radius * 2));
                bool ptHovered = ImGui::IsItemHovered();
                bool ptActive  = ImGui::IsItemActive();

                if (ptActive && ImGui::IsMouseDragging(0)) {
                    ImVec2 delta = ImGui::GetIO().MouseDelta;
                    points[i].first  += delta.x / canvasSize.x;
                    points[i].second -= delta.y / canvasSize.y;
                    // Clamp endpoints
                    if (i == 0) points[i].first = 0.0f;
                    if (i == points.size() - 1) points[i].first = 1.0f;
                    points[i].first  = std::clamp(points[i].first,  0.0f, 1.0f);
                    points[i].second = std::clamp(points[i].second, 0.0f, 1.0f);
                    dragIdx = static_cast<int>(i);
                }

                dl->AddCircleFilled(ptScreen, radius,
                    (ptHovered || ptActive) ? IM_COL32(255, 255, 0, 255) : curveColor);
                ImGui::PopID();
            }

            // Sort points by x after drag
            if (dragIdx >= 0) {
                std::sort(points.begin(), points.end(),
                    [](const auto& a, const auto& b) { return a.first < b.first; });
            }

            // Double-click to add point
            if (canvasHovered && ImGui::IsMouseDoubleClicked(0)) {
                ImVec2 mousePos = ImGui::GetIO().MousePos;
                float nx = (mousePos.x - canvasPos.x) / canvasSize.x;
                float ny = 1.0f - (mousePos.y - canvasPos.y) / canvasSize.y;
                nx = std::clamp(nx, 0.0f, 1.0f);
                ny = std::clamp(ny, 0.0f, 1.0f);
                points.push_back({ nx, ny });
                std::sort(points.begin(), points.end(),
                    [](const auto& a, const auto& b) { return a.first < b.first; });
            }

            // Right-click to remove point (not endpoints)
            if (canvasHovered && ImGui::IsMouseClicked(1)) {
                ImVec2 mousePos = ImGui::GetIO().MousePos;
                for (size_t i = 1; i + 1 < points.size(); i++) {
                    ImVec2 ptScreen(canvasPos.x + points[i].first * canvasSize.x,
                                    canvasPos.y + (1.0f - points[i].second) * canvasSize.y);
                    float dx = mousePos.x - ptScreen.x;
                    float dy = mousePos.y - ptScreen.y;
                    if (dx * dx + dy * dy < 64.0f) {
                        points.erase(points.begin() + i);
                        break;
                    }
                }
            }

            if (ImGui::Button("Reset##curve")) {
                points = { {0.0f, 0.0f}, {1.0f, 1.0f} };
            }

            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    struct TextureProbeSummary {
        bool Valid = false;
        uint32_t Width = 0;
        uint32_t Height = 0;
        float AverageLuminance = 0.0f;
        float AverageAlpha = 0.0f;
        std::array<float, 4> AverageRGBA = { 0.0f, 0.0f, 0.0f, 0.0f };
        float NonBlackRatio = 0.0f;
        std::array<unsigned char, 4> Center = { 0, 0, 0, 0 };
        std::array<unsigned char, 4> MaxRGBA = { 0, 0, 0, 0 };
    };

    struct TextureFloatProbeSummary {
        bool Valid = false;
        uint32_t Width = 0;
        uint32_t Height = 0;
        std::array<float, 4> AverageRGBA = { 0.0f, 0.0f, 0.0f, 0.0f };
        std::array<float, 4> MinRGBA = {
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max()
        };
        std::array<float, 4> MaxRGBA = {
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest()
        };
        std::array<float, 4> Center = { 0.0f, 0.0f, 0.0f, 0.0f };
        float NonZeroRGBRatio = 0.0f;
        float NonZeroAlphaRatio = 0.0f;
    };

    TextureProbeSummary ProbeTexture(uint32_t textureID, uint32_t width, uint32_t height) {
        TextureProbeSummary summary;
        summary.Width = width;
        summary.Height = height;
        if (textureID == 0 || width == 0 || height == 0)
            return summary;

        GLint previousTexture = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);

        std::vector<unsigned char> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
        glBindTexture(GL_TEXTURE_2D, textureID);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));

        const size_t centerIndex = ((static_cast<size_t>(height / 2) * width) + (width / 2)) * 4;
        if (centerIndex + 3 < pixels.size()) {
            summary.Center = {
                pixels[centerIndex + 0],
                pixels[centerIndex + 1],
                pixels[centerIndex + 2],
                pixels[centerIndex + 3]
            };
        }

        double luminanceSum = 0.0;
        double alphaSum = 0.0;
        std::array<double, 4> channelSum = { 0.0, 0.0, 0.0, 0.0 };
        size_t nonBlack = 0;
        const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
        const size_t step = std::max<size_t>(1, pixelCount / 65536);
        size_t sampled = 0;
        for (size_t pixel = 0; pixel < pixelCount; pixel += step) {
            const size_t i = pixel * 4;
            const unsigned char r = pixels[i + 0];
            const unsigned char g = pixels[i + 1];
            const unsigned char b = pixels[i + 2];
            const unsigned char a = pixels[i + 3];
            summary.MaxRGBA[0] = std::max(summary.MaxRGBA[0], r);
            summary.MaxRGBA[1] = std::max(summary.MaxRGBA[1], g);
            summary.MaxRGBA[2] = std::max(summary.MaxRGBA[2], b);
            summary.MaxRGBA[3] = std::max(summary.MaxRGBA[3], a);
            luminanceSum += 0.2126 * r + 0.7152 * g + 0.0722 * b;
            alphaSum += a;
            channelSum[0] += r;
            channelSum[1] += g;
            channelSum[2] += b;
            channelSum[3] += a;
            if (r > 2 || g > 2 || b > 2)
                nonBlack++;
            sampled++;
        }

        summary.Valid = sampled > 0;
        if (sampled > 0) {
            summary.AverageLuminance = static_cast<float>(luminanceSum / (255.0 * sampled));
            summary.AverageAlpha = static_cast<float>(alphaSum / (255.0 * sampled));
            summary.AverageRGBA = {
                static_cast<float>(channelSum[0] / (255.0 * sampled)),
                static_cast<float>(channelSum[1] / (255.0 * sampled)),
                static_cast<float>(channelSum[2] / (255.0 * sampled)),
                static_cast<float>(channelSum[3] / (255.0 * sampled))
            };
            summary.NonBlackRatio = static_cast<float>(static_cast<double>(nonBlack) / sampled);
        }
        return summary;
    }

    TextureFloatProbeSummary ProbeTextureFloat(uint32_t textureID, uint32_t width, uint32_t height) {
        TextureFloatProbeSummary summary;
        summary.Width = width;
        summary.Height = height;
        if (textureID == 0 || width == 0 || height == 0)
            return summary;

        GLint previousTexture = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);

        std::vector<float> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
        glBindTexture(GL_TEXTURE_2D, textureID);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, pixels.data());
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));

        const size_t centerIndex = ((static_cast<size_t>(height / 2) * width) + (width / 2)) * 4;
        if (centerIndex + 3 < pixels.size()) {
            summary.Center = {
                pixels[centerIndex + 0],
                pixels[centerIndex + 1],
                pixels[centerIndex + 2],
                pixels[centerIndex + 3]
            };
        }

        std::array<double, 4> channelSum = { 0.0, 0.0, 0.0, 0.0 };
        size_t nonZeroRGB = 0;
        size_t nonZeroAlpha = 0;
        const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
        const size_t step = std::max<size_t>(1, pixelCount / 65536);
        size_t sampled = 0;
        for (size_t pixel = 0; pixel < pixelCount; pixel += step) {
            const size_t i = pixel * 4;
            const std::array<float, 4> rgba = {
                pixels[i + 0],
                pixels[i + 1],
                pixels[i + 2],
                pixels[i + 3]
            };
            for (size_t c = 0; c < rgba.size(); ++c) {
                const float value = std::isfinite(rgba[c]) ? rgba[c] : 0.0f;
                summary.MinRGBA[c] = std::min(summary.MinRGBA[c], value);
                summary.MaxRGBA[c] = std::max(summary.MaxRGBA[c], value);
                channelSum[c] += value;
            }
            if (std::abs(rgba[0]) > 0.00001f || std::abs(rgba[1]) > 0.00001f || std::abs(rgba[2]) > 0.00001f)
                nonZeroRGB++;
            if (std::abs(rgba[3]) > 0.00001f)
                nonZeroAlpha++;
            sampled++;
        }

        summary.Valid = sampled > 0;
        if (sampled > 0) {
            summary.AverageRGBA = {
                static_cast<float>(channelSum[0] / sampled),
                static_cast<float>(channelSum[1] / sampled),
                static_cast<float>(channelSum[2] / sampled),
                static_cast<float>(channelSum[3] / sampled)
            };
            summary.NonZeroRGBRatio = static_cast<float>(static_cast<double>(nonZeroRGB) / sampled);
            summary.NonZeroAlphaRatio = static_cast<float>(static_cast<double>(nonZeroAlpha) / sampled);
        }
        return summary;
    }

    bool SaveTextureBMP(uint32_t textureID, uint32_t width, uint32_t height, const std::filesystem::path& path) {
        if (textureID == 0 || width == 0 || height == 0)
            return false;

        GLint previousTexture = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);

        std::vector<unsigned char> rgba(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
        glBindTexture(GL_TEXTURE_2D, textureID);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));

        const uint32_t rowStride = ((width * 3 + 3) / 4) * 4;
        const uint32_t pixelDataSize = rowStride * height;
        const uint32_t fileSize = 54 + pixelDataSize;
        std::vector<unsigned char> bmp(fileSize, 0);

        bmp[0] = 'B';
        bmp[1] = 'M';
        auto writeU32 = [&](size_t offset, uint32_t value) {
            bmp[offset + 0] = static_cast<unsigned char>(value & 0xff);
            bmp[offset + 1] = static_cast<unsigned char>((value >> 8) & 0xff);
            bmp[offset + 2] = static_cast<unsigned char>((value >> 16) & 0xff);
            bmp[offset + 3] = static_cast<unsigned char>((value >> 24) & 0xff);
        };
        auto writeU16 = [&](size_t offset, uint16_t value) {
            bmp[offset + 0] = static_cast<unsigned char>(value & 0xff);
            bmp[offset + 1] = static_cast<unsigned char>((value >> 8) & 0xff);
        };

        writeU32(2, fileSize);
        writeU32(10, 54);
        writeU32(14, 40);
        writeU32(18, width);
        writeU32(22, height);
        writeU16(26, 1);
        writeU16(28, 24);
        writeU32(34, pixelDataSize);

        for (uint32_t y = 0; y < height; ++y) {
            const uint32_t srcY = y;
            unsigned char* dst = bmp.data() + 54 + static_cast<size_t>(y) * rowStride;
            for (uint32_t x = 0; x < width; ++x) {
                const size_t src = (static_cast<size_t>(srcY) * width + x) * 4;
                dst[x * 3 + 0] = rgba[src + 2];
                dst[x * 3 + 1] = rgba[src + 1];
                dst[x * 3 + 2] = rgba[src + 0];
            }
        }

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;
        file.write(reinterpret_cast<const char*>(bmp.data()), static_cast<std::streamsize>(bmp.size()));
        return true;
    }

    void WriteRenderDiagnosticsFile() {
        const std::filesystem::path outputPath = std::filesystem::path(VE_PROJECT_ROOT) / "render_diagnostics.txt";
        std::ofstream out(outputPath, std::ios::trunc);
        if (!out)
            return;

        const auto& d = m_Scene->GetRenderDiagnostics();
        auto& dr = m_Scene->GetDeferredRenderer();

        out << "VibeEngine Render Diagnostics\n";
        out << "ScenePath: " << m_CurrentScenePath << "\n";
        out << "FrameIndex: " << d.FrameIndex << "\n";
        out << "Viewport: " << d.ViewportWidth << "x" << d.ViewportHeight << "\n";
        out << "DeferredInitialized: " << d.DeferredInitialized << "\n";
        out << "LightingPassRan: " << d.LightingPassRan << "\n";
        out << "ForwardPassRan: " << d.ForwardPassRan << "\n";
        out << "DeferredOutputTexture: " << d.DeferredOutputTexture << "\n";
        out << "MeshRendererEntities: " << d.MeshRendererEntities << "\n";
        out << "OpaqueSubmitted: " << d.OpaqueSubmitted << "\n";
        out << "TransparentQueued: " << d.TransparentQueued << "\n";
        out << "TransparentDrawn: " << d.TransparentDrawn << "\n";
        out << "FrustumCulled: " << d.FrustumCulled << "\n";
        out << "HPWaterEntities: " << d.HPWaterEntities << "\n";
        out << "HPWaterWithMesh: " << d.HPWaterWithMesh << "\n";
        out << "HPWaterQueued: " << d.HPWaterQueued << "\n";
        out << "HPWaterDrawn: " << d.HPWaterDrawn << "\n";
        out << "HPWaterCulled: " << d.HPWaterCulled << "\n";
        out << "HPWaterGBufferDrawn: " << d.HPWaterGBufferDrawn << "\n";
        out << "HPWaterCompositeRan: " << d.HPWaterCompositeRan << "\n";
        out << "HPWaterGBufferInitialized: " << d.HPWaterGBufferInitialized << "\n";
        out << "HPWaterGBufferAttachmentCount: " << d.HPWaterGBufferAttachmentCount << "\n";
        out << "HPWaterGBuffer0: " << d.HPWaterGBuffer0 << "\n";
        out << "HPWaterGBuffer1: " << d.HPWaterGBuffer1 << "\n";
        out << "HPWaterGBuffer2: " << d.HPWaterGBuffer2 << "\n";
        out << "HPWaterGBufferDepth: " << d.HPWaterGBufferDepth << "\n";
        out << "HPWaterMaskRan: " << d.HPWaterMaskRan << "\n";
        out << "HPWaterMaskTexture: " << d.HPWaterMaskTexture << "\n";
        out << "HPWaterMaskSize: " << d.HPWaterMaskWidth << "x" << d.HPWaterMaskHeight << "\n";
        out << "HPWaterCompositeTexture: " << d.HPWaterCompositeTexture << "\n";
        out << "HPWaterRefractionDataTexture: " << d.HPWaterRefractionDataTexture << "\n";
        out << "HPWaterRefractionMetaTexture: " << d.HPWaterRefractionMetaTexture << "\n";
        out << "HPWaterDepthPyramidRan: " << d.HPWaterDepthPyramidRan << "\n";
        out << "HPWaterDepthMergedToSceneDepth: " << d.HPWaterDepthMergedToSceneDepth << "\n";
        out << "HPWaterNormalMergedToSceneGBuffer: " << d.HPWaterNormalMergedToSceneGBuffer << "\n";
        out << "HPWaterStencilMarkedInSceneDepth: " << d.HPWaterStencilMarkedInSceneDepth << "\n";
        out << "HPWaterStencilRef: " << d.HPWaterStencilRef << "\n";
        out << "HPWaterDepthPyramidTexture: " << d.HPWaterDepthPyramidTexture << "\n";
        out << "HPWaterDepthPyramidMipCount: " << d.HPWaterDepthPyramidMipCount << "\n";
        out << "HPWaterDepthPyramidSize: " << d.HPWaterDepthPyramidWidth << "x" << d.HPWaterDepthPyramidHeight << "\n";
        out << "HPWaterRefractionStrength: " << d.HPWaterRefractionStrength << "\n";
        out << "HPWaterWaterDispersionStrength: " << d.HPWaterWaterDispersionStrength << "\n";
        out << "HPWaterMaxRefractionCrossDistance: " << d.HPWaterMaxRefractionCrossDistance << "\n";
        out << "HPWaterRefractionThicknessOffset: " << d.HPWaterRefractionThicknessOffset << "\n";
        out << "HPWaterRefractionSampleCount: " << d.HPWaterRefractionSampleCount << "\n";
        out << "HPWaterRefractionJitterEnabled: " << d.HPWaterRefractionJitterEnabled << "\n";
        out << "HPWaterRefractionNDCMarchEnabled: " << d.HPWaterRefractionNDCMarchEnabled << "\n";
        out << "HPWaterEnvironmentReflectionIntensity: " << d.HPWaterEnvironmentReflectionIntensity << "\n";
        out << "HPWaterIndirectLightStrength: " << d.HPWaterIndirectLightStrength << "\n";
        out << "HPWaterMacroScatterStrength: " << d.HPWaterMacroScatterStrength << "\n";
        out << "HPWaterThinSSSStrength: " << d.HPWaterThinSSSStrength << "\n";
        out << "HPWaterBacklitTransmissionStrength: " << d.HPWaterBacklitTransmissionStrength << "\n";
        out << "HPWaterForwardScatterStrength: " << d.HPWaterForwardScatterStrength << "\n";
        out << "HPWaterForwardScatterBlurDensity: " << d.HPWaterForwardScatterBlurDensity << "\n";
        out << "HPWaterMultiScatterScale: " << d.HPWaterMultiScatterScale << "\n";
        out << "HPWaterPhaseG: " << d.HPWaterPhaseG << "\n";
        out << "HPWaterSpecularFGDStrength: " << d.HPWaterSpecularFGDStrength << "\n";
        out << "HPWaterGGXEnergyCompensation: " << d.HPWaterGGXEnergyCompensation << "\n";
        out << "HPWaterPreintegratedFGDLUTValid: " << d.HPWaterPreintegratedFGDLUTValid << "\n";
        out << "HPWaterPreintegratedFGDLUTTexture: " << d.HPWaterPreintegratedFGDLUTTexture << "\n";
        out << "HPWaterPreintegratedFGDLUTResolution: " << d.HPWaterPreintegratedFGDLUTResolution << "\n";
        out << "HPWaterAreaLightLTCLUTValid: " << d.HPWaterAreaLightLTCLUTValid << "\n";
        out << "HPWaterAreaLightLTCLUTTexture: " << d.HPWaterAreaLightLTCLUTTexture << "\n";
        out << "HPWaterAreaLightLTCLUTResolution: " << d.HPWaterAreaLightLTCLUTResolution << "\n";
        out << "HPWaterAreaLightLTCLUTLayers: " << d.HPWaterAreaLightLTCLUTLayers << "\n";
        out << "HPWaterAreaLightLTCHDRPTableEnabled: "
            << d.HPWaterAreaLightLTCHDRPTableEnabled << "\n";
        out << "HPWaterAreaLightLTCSamplingEnabled: " << d.HPWaterAreaLightLTCSamplingEnabled << "\n";
        out << "HPWaterAreaLightLTCHDRPUVEnabled: " << d.HPWaterAreaLightLTCHDRPUVEnabled << "\n";
        out << "HPWaterAreaLightLTCCosThetaParamEnabled: "
            << d.HPWaterAreaLightLTCCosThetaParamEnabled << "\n";
        out << "HPWaterAreaLightLTCMatrixCoefficientsEnabled: "
            << d.HPWaterAreaLightLTCMatrixCoefficientsEnabled << "\n";
        out << "HPWaterAreaLightLTCPolygonIntegrationEnabled: "
            << d.HPWaterAreaLightLTCPolygonIntegrationEnabled << "\n";
        out << "HPWaterAreaLightLTCHorizonClippingEnabled: "
            << d.HPWaterAreaLightLTCHorizonClippingEnabled << "\n";
        out << "HPWaterLightLoopInputsValid: " << d.HPWaterLightLoopInputsValid << "\n";
        out << "HPWaterSurfaceShadowSamplingEnabled: " << d.HPWaterSurfaceShadowSamplingEnabled << "\n";
        out << "HPWaterShadowCascadeDitherEnabled: " << d.HPWaterShadowCascadeDitherEnabled << "\n";
        out << "HPWaterIndirectScatterIntegrationEnabled: " << d.HPWaterIndirectScatterIntegrationEnabled << "\n";
        out << "HPWaterBSDFComponentWeightingEnabled: " << d.HPWaterBSDFComponentWeightingEnabled << "\n";
        out << "HPWaterPunctualBodyComponentWeightingEnabled: "
            << d.HPWaterPunctualBodyComponentWeightingEnabled << "\n";
        out << "HPWaterSpecularSelfOcclusionEnabled: "
            << d.HPWaterSpecularSelfOcclusionEnabled << "\n";
        out << "HPWaterExitFresnelEnabled: " << d.HPWaterExitFresnelEnabled << "\n";
        out << "HPWaterExitFresnelF0: " << d.HPWaterExitFresnelF0 << "\n";
        out << "HPWaterSkyReflectionIntensity: " << d.HPWaterSkyReflectionIntensity << "\n";
        out << "HPWaterIndirectDiffuseIntensity: " << d.HPWaterIndirectDiffuseIntensity << "\n";
        out << "HPWaterDirectionalLightIntensity: " << d.HPWaterDirectionalLightIntensity << "\n";
        out << "HPWaterPointLightCount: " << d.HPWaterPointLightCount << "\n";
        out << "HPWaterSpotLightCount: " << d.HPWaterSpotLightCount << "\n";
        out << "HPWaterAreaLightCount: " << d.HPWaterAreaLightCount << "\n";
        out << "HPWaterPunctualLightLoopEnabled: " << d.HPWaterPunctualLightLoopEnabled << "\n";
        out << "HPWaterAreaLightApproximationEnabled: "
            << d.HPWaterAreaLightApproximationEnabled << "\n";
        out << "HPWaterAreaLightRectangleSamplingEnabled: "
            << d.HPWaterAreaLightRectangleSamplingEnabled << "\n";
        out << "HPWaterPunctualLightLayerFilteringEnabled: "
            << d.HPWaterPunctualLightLayerFilteringEnabled << "\n";
        out << "HPWaterAreaLightLayerFilteringEnabled: "
            << d.HPWaterAreaLightLayerFilteringEnabled << "\n";
        out << "HPWaterPunctualLightInfluenceSortingEnabled: "
            << d.HPWaterPunctualLightInfluenceSortingEnabled << "\n";
        out << "HPWaterPunctualPointLightCandidates: "
            << d.HPWaterPunctualPointLightCandidates << "\n";
        out << "HPWaterPunctualSpotLightCandidates: "
            << d.HPWaterPunctualSpotLightCandidates << "\n";
        out << "HPWaterAreaLightCandidates: "
            << d.HPWaterAreaLightCandidates << "\n";
        out << "HPWaterPunctualPointLightCapacity: "
            << d.HPWaterPunctualPointLightCapacity << "\n";
        out << "HPWaterPunctualSpotLightCapacity: "
            << d.HPWaterPunctualSpotLightCapacity << "\n";
        out << "HPWaterAreaLightCapacity: "
            << d.HPWaterAreaLightCapacity << "\n";
        out << "HPWaterLightSelectionBoundsValid: "
            << d.HPWaterLightSelectionBoundsValid << "\n";
        out << "HPWaterLightSelectionCenter: "
            << d.HPWaterLightSelectionCenter.x << ","
            << d.HPWaterLightSelectionCenter.y << ","
            << d.HPWaterLightSelectionCenter.z << "\n";
        out << "HPWaterLightSelectionRadius: "
            << d.HPWaterLightSelectionRadius << "\n";
        out << "HPWaterPointLightTopInfluenceScore: "
            << d.HPWaterPointLightTopInfluenceScore << "\n";
        out << "HPWaterSpotLightTopInfluenceScore: "
            << d.HPWaterSpotLightTopInfluenceScore << "\n";
        out << "HPWaterAreaLightTopInfluenceScore: "
            << d.HPWaterAreaLightTopInfluenceScore << "\n";
        out << "HPWaterPointLightSelectedInfluenceSum: "
            << d.HPWaterPointLightSelectedInfluenceSum << "\n";
        out << "HPWaterSpotLightSelectedInfluenceSum: "
            << d.HPWaterSpotLightSelectedInfluenceSum << "\n";
        out << "HPWaterAreaLightSelectedInfluenceSum: "
            << d.HPWaterAreaLightSelectedInfluenceSum << "\n";
        out << "HPWaterPunctualLightsLayerSkipped: "
            << d.HPWaterPunctualLightsLayerSkipped << "\n";
        out << "HPWaterPunctualLightsCapacitySkipped: "
            << d.HPWaterPunctualLightsCapacitySkipped << "\n";
        out << "HPWaterPunctualPointLightsCapacitySkipped: "
            << d.HPWaterPunctualPointLightsCapacitySkipped << "\n";
        out << "HPWaterPunctualSpotLightsCapacitySkipped: "
            << d.HPWaterPunctualSpotLightsCapacitySkipped << "\n";
        out << "HPWaterAreaLightsLayerSkipped: "
            << d.HPWaterAreaLightsLayerSkipped << "\n";
        out << "HPWaterAreaLightsCapacitySkipped: "
            << d.HPWaterAreaLightsCapacitySkipped << "\n";
        out << "HPWaterVolumePointLightCount: " << d.HPWaterVolumePointLightCount << "\n";
        out << "HPWaterVolumeSpotLightCount: " << d.HPWaterVolumeSpotLightCount << "\n";
        out << "HPWaterVolumeAreaLightCount: " << d.HPWaterVolumeAreaLightCount << "\n";
        out << "HPWaterVolumePunctualLightLoopEnabled: "
            << d.HPWaterVolumePunctualLightLoopEnabled << "\n";
        out << "HPWaterVolumeAreaLightLoopEnabled: "
            << d.HPWaterVolumeAreaLightLoopEnabled << "\n";
        out << "HPWaterVolumeAreaLightRectangleSamplingEnabled: "
            << d.HPWaterVolumeAreaLightRectangleSamplingEnabled << "\n";
        out << "HPWaterVolumeAreaLightLTCPolygonIntegrationEnabled: "
            << d.HPWaterVolumeAreaLightLTCPolygonIntegrationEnabled << "\n";
        out << "HPWaterVolumeAreaLightLTCHorizonClippingEnabled: "
            << d.HPWaterVolumeAreaLightLTCHorizonClippingEnabled << "\n";
        out << "HPWaterSpectralOceanEnabled: " << d.HPWaterSpectralOceanEnabled << "\n";
        out << "HPWaterSpectralNormalParityEnabled: " << d.HPWaterSpectralNormalParityEnabled << "\n";
        out << "HPWaterSpectrumComputeRan: " << d.HPWaterSpectrumComputeRan << "\n";
        out << "HPWaterSpectrumComputeValid: " << d.HPWaterSpectrumComputeValid << "\n";
        out << "HPWaterSpectrumTextureConsumed: " << d.HPWaterSpectrumTextureConsumed << "\n";
        out << "HPWaterSpectrumTexture: " << d.HPWaterSpectrumTexture << "\n";
        out << "HPWaterSpectrumResolution: " << d.HPWaterSpectrumResolution << "\n";
        out << "HPWaterSpectrumAmplitude: " << d.HPWaterSpectrumAmplitude << "\n";
        out << "HPWaterSpectrumNormalStrength: " << d.HPWaterSpectrumNormalStrength << "\n";
        out << "HPWaterSpectrumWindSpeed: " << d.HPWaterSpectrumWindSpeed << "\n";
        out << "HPWaterSpectrumDirectionalSpread: " << d.HPWaterSpectrumDirectionalSpread << "\n";
        out << "HPWaterSpectrumSwell: " << d.HPWaterSpectrumSwell << "\n";
        out << "HPWaterSpectrumShortWaveFade: " << d.HPWaterSpectrumShortWaveFade << "\n";
        out << "HPWaterSpectrumWindModelEnabled: " << d.HPWaterSpectrumWindModelEnabled << "\n";
        out << "HPWaterSpectrumFrequencyDomainEnabled: "
            << d.HPWaterSpectrumFrequencyDomainEnabled << "\n";
        out << "HPWaterSpectrumPhillipsEnabled: " << d.HPWaterSpectrumPhillipsEnabled << "\n";
        out << "HPWaterSpectrumJonswapEnabled: " << d.HPWaterSpectrumJonswapEnabled << "\n";
        out << "HPWaterSpectrumIFFTEnabled: " << d.HPWaterSpectrumIFFTEnabled << "\n";
        out << "HPWaterSpectrumButterflyPasses: " << d.HPWaterSpectrumButterflyPasses << "\n";
        out << "HPWaterSkyTextureReflectionBound: " << d.HPWaterSkyTextureReflectionBound << "\n";
        out << "HPWaterSkyTexture: " << d.HPWaterSkyTexture << "\n";
        out << "HPWaterReflectionProbeBound: " << d.HPWaterReflectionProbeBound << "\n";
        out << "HPWaterReflectionProbeTexture: " << d.HPWaterReflectionProbeTexture << "\n";
        out << "HPWaterReflectionProbeSecondaryTexture: " << d.HPWaterReflectionProbeSecondaryTexture << "\n";
        out << "HPWaterReflectionProbeIntensity: " << d.HPWaterReflectionProbeIntensity << "\n";
        out << "HPWaterReflectionProbeBlend: " << d.HPWaterReflectionProbeBlend << "\n";
        out << "HPWaterReflectionProbeInfluenceWeight: " << d.HPWaterReflectionProbeInfluenceWeight << "\n";
        out << "HPWaterReflectionProbeHierarchyWeight: " << d.HPWaterReflectionProbeHierarchyWeight << "\n";
        out << "HPWaterReflectionProbeInfluenceBlendEnabled: "
            << d.HPWaterReflectionProbeInfluenceBlendEnabled << "\n";
        out << "HPWaterReflectionProbeBoxProjectionEnabled: "
            << d.HPWaterReflectionProbeBoxProjectionEnabled << "\n";
        out << "HPWaterEnvSpecularDominantDirEnabled: "
            << d.HPWaterEnvSpecularDominantDirEnabled << "\n";
        out << "HPWaterEnvSpecularDominantDirExactFormulaEnabled: "
            << d.HPWaterEnvSpecularDominantDirExactFormulaEnabled << "\n";
        out << "HPWaterEnvSpecularMultiBounceEnabled: "
            << d.HPWaterEnvSpecularMultiBounceEnabled << "\n";
        out << "HPWaterSSRReflectionEnabled: " << d.HPWaterSSRReflectionEnabled << "\n";
        out << "HPWaterSSRHierarchyBlendEnabled: " << d.HPWaterSSRHierarchyBlendEnabled << "\n";
        out << "HPWaterSSRLightingBufferRan: " << d.HPWaterSSRLightingBufferRan << "\n";
        out << "HPWaterSSRLightingBufferValid: " << d.HPWaterSSRLightingBufferValid << "\n";
        out << "HPWaterSSRLightingRGBPreweighted: " << d.HPWaterSSRLightingRGBPreweighted << "\n";
        out << "HPWaterSSRHitRefinementEnabled: " << d.HPWaterSSRHitRefinementEnabled << "\n";
        out << "HPWaterSSRRoughnessConeTracingEnabled: "
            << d.HPWaterSSRRoughnessConeTracingEnabled << "\n";
        out << "HPWaterSSRTemporalResolveEnabled: "
            << d.HPWaterSSRTemporalResolveEnabled << "\n";
        out << "HPWaterSSRHistoryValid: " << d.HPWaterSSRHistoryValid << "\n";
        out << "HPWaterSSRExplicitMotionVectorEnabled: "
            << d.HPWaterSSRExplicitMotionVectorEnabled << "\n";
        out << "HPWaterSSRMotionVectorHistoryEnabled: "
            << d.HPWaterSSRMotionVectorHistoryEnabled << "\n";
        out << "HPWaterSSRMotionReprojectionEnabled: "
            << d.HPWaterSSRMotionReprojectionEnabled << "\n";
        out << "HPWaterSSRDisocclusionRejectionEnabled: "
            << d.HPWaterSSRDisocclusionRejectionEnabled << "\n";
        out << "HPWaterCompositeConsumesSSRLightingBuffer: "
            << d.HPWaterCompositeConsumesSSRLightingBuffer << "\n";
        out << "HPWaterSSRLightingBufferTexture: " << d.HPWaterSSRLightingBufferTexture << "\n";
        out << "HPWaterSSRDiagnosticsValid: " << d.HPWaterSSRDiagnosticsValid << "\n";
        out << "HPWaterSSRDiagnosticsTexture: " << d.HPWaterSSRDiagnosticsTexture << "\n";
        out << "HPWaterSSRMaxSteps: " << d.HPWaterSSRMaxSteps << "\n";
        out << "HPWaterSSRStepSize: " << d.HPWaterSSRStepSize << "\n";
        out << "HPWaterSSRThickness: " << d.HPWaterSSRThickness << "\n";
        out << "HPWaterSSRMaxDistance: " << d.HPWaterSSRMaxDistance << "\n";
        out << "HPWaterReflectionProbeCenter: "
            << d.HPWaterReflectionProbeCenter.x << ","
            << d.HPWaterReflectionProbeCenter.y << ","
            << d.HPWaterReflectionProbeCenter.z << "\n";
        out << "HPWaterReflectionProbeBoxSize: "
            << d.HPWaterReflectionProbeBoxSize.x << ","
            << d.HPWaterReflectionProbeBoxSize.y << ","
            << d.HPWaterReflectionProbeBoxSize.z << "\n";
        out << "HPWaterReflectionProbeSecondaryCenter: "
            << d.HPWaterReflectionProbeSecondaryCenter.x << ","
            << d.HPWaterReflectionProbeSecondaryCenter.y << ","
            << d.HPWaterReflectionProbeSecondaryCenter.z << "\n";
        out << "HPWaterReflectionProbeSecondaryBoxSize: "
            << d.HPWaterReflectionProbeSecondaryBoxSize.x << ","
            << d.HPWaterReflectionProbeSecondaryBoxSize.y << ","
            << d.HPWaterReflectionProbeSecondaryBoxSize.z << "\n";
        out << "HPWaterForwardScatterMipEnabled: " << d.HPWaterForwardScatterMipEnabled << "\n";
        out << "HPWaterForwardScatterMipCount: " << d.HPWaterForwardScatterMipCount << "\n";
        out << "HPWaterVolumeRan: " << d.HPWaterVolumeRan << "\n";
        out << "HPWaterVolumeColorTexture: " << d.HPWaterVolumeColorTexture << "\n";
        out << "HPWaterVolumeTransmittanceTexture: " << d.HPWaterVolumeTransmittanceTexture << "\n";
        out << "HPWaterVolumeDepthTexture: " << d.HPWaterVolumeDepthTexture << "\n";
        out << "HPWaterVolumeSize: " << d.HPWaterVolumeWidth << "x" << d.HPWaterVolumeHeight << "\n";
        out << "HPWaterVolumeTemporalRan: " << d.HPWaterVolumeTemporalRan << "\n";
        out << "HPWaterVolumeTemporalNeighborhoodClampEnabled: "
            << d.HPWaterVolumeTemporalNeighborhoodClampEnabled << "\n";
        out << "HPWaterVolumeTemporalMotionReprojectionEnabled: "
            << d.HPWaterVolumeTemporalMotionReprojectionEnabled << "\n";
        out << "HPWaterVolumeExplicitMotionVectorEnabled: "
            << d.HPWaterVolumeExplicitMotionVectorEnabled << "\n";
        out << "HPWaterVolumeSceneMotionVectorEnabled: "
            << d.HPWaterVolumeSceneMotionVectorEnabled << "\n";
        out << "HPWaterVolumeObjectMotionVectorEnabled: "
            << d.HPWaterVolumeObjectMotionVectorEnabled << "\n";
        out << "HPWaterVolumeObjectMotionFieldEnabled: "
            << d.HPWaterVolumeObjectMotionFieldEnabled << "\n";
        out << "HPWaterVolumeObjectMotionIDSelectionEnabled: "
            << d.HPWaterVolumeObjectMotionIDSelectionEnabled << "\n";
        out << "HPWaterVolumeObjectMotionWorldOffset: "
            << d.HPWaterVolumeObjectMotionWorldOffset.x << " "
            << d.HPWaterVolumeObjectMotionWorldOffset.y << " "
            << d.HPWaterVolumeObjectMotionWorldOffset.z << "\n";
        out << "HPWaterVolumeObjectMotionSourceCount: "
            << d.HPWaterVolumeObjectMotionSourceCount << "\n";
        out << "HPWaterVolumeObjectMotionTrackedCount: "
            << d.HPWaterVolumeObjectMotionTrackedCount << "\n";
        out << "HPWaterVolumeObjectMotionMatchedCount: "
            << d.HPWaterVolumeObjectMotionMatchedCount << "\n";
        out << "HPWaterVolumeObjectMotionFieldCapacity: "
            << d.HPWaterVolumeObjectMotionFieldCapacity << "\n";
        out << "HPWaterVolumeObjectMotionFieldSelected: "
            << d.HPWaterVolumeObjectMotionFieldSelected << "\n";
        out << "HPWaterVolumeMotionVectorHistoryEnabled: "
            << d.HPWaterVolumeMotionVectorHistoryEnabled << "\n";
        out << "HPWaterVolumeExponentialIntegrationEnabled: "
            << d.HPWaterVolumeExponentialIntegrationEnabled << "\n";
        out << "HPWaterVolumeSceneInScatteringEnabled: "
            << d.HPWaterVolumeSceneInScatteringEnabled << "\n";
        out << "HPWaterVolumeAlbedoPhaseBlendEnabled: "
            << d.HPWaterVolumeAlbedoPhaseBlendEnabled << "\n";
        out << "HPWaterVolumePhaseGEnabled: "
            << d.HPWaterVolumePhaseGEnabled << "\n";
        out << "HPWaterVolumeShadowSamplingEnabled: "
            << d.HPWaterVolumeShadowSamplingEnabled << "\n";
        out << "HPWaterVolumeShadowParamsEnabled: "
            << d.HPWaterVolumeShadowParamsEnabled << "\n";
        out << "HPWaterVolumeMaxCrossDistanceEnabled: "
            << d.HPWaterVolumeMaxCrossDistanceEnabled << "\n";
        out << "HPWaterVolumeDynamicShadowDistanceEnabled: "
            << d.HPWaterVolumeDynamicShadowDistanceEnabled << "\n";
        out << "HPWaterVolumeSampleCount: "
            << d.HPWaterVolumeSampleCount << "\n";
        out << "HPWaterVolumeMaxCrossDistance: "
            << d.HPWaterVolumeMaxCrossDistance << "\n";
        out << "HPWaterVolumeShadowSoftness: "
            << d.HPWaterVolumeShadowSoftness << "\n";
        out << "HPWaterVolumeShadowMinFilterSize: "
            << d.HPWaterVolumeShadowMinFilterSize << "\n";
        out << "HPWaterVolumeShadowBlockerSamples: "
            << d.HPWaterVolumeShadowBlockerSamples << "\n";
        out << "HPWaterVolumeShadowFilterSamples: "
            << d.HPWaterVolumeShadowFilterSamples << "\n";
        out << "HPWaterVolumeMotionVectorTexture: "
            << d.HPWaterVolumeMotionVectorTexture << "\n";
        out << "HPWaterVolumeTemporalBlendFactor: "
            << d.HPWaterVolumeTemporalBlendFactor << "\n";
        out << "HPWaterVolumeSpatialFilterEnabled: "
            << d.HPWaterVolumeSpatialFilterEnabled << "\n";
        out << "HPWaterVolumeSpatialFilterIterations: "
            << d.HPWaterVolumeSpatialFilterIterations << "\n";
        out << "HPWaterVolumeMotionVectorsEnabled: "
            << d.HPWaterVolumeMotionVectorsEnabled << "\n";
        out << "HPWaterVolumeMotionVectorVelocityScale: "
            << d.HPWaterVolumeMotionVectorVelocityScale << "\n";
        out << "HPWaterVolumeTemporalDepthRejectionEnabled: "
            << d.HPWaterVolumeTemporalDepthRejectionEnabled << "\n";
        out << "HPWaterVolumeTemporalDepthThreshold: "
            << d.HPWaterVolumeTemporalDepthThreshold << "\n";
        out << "HPWaterVolumeSpatialDepthAwareEnabled: "
            << d.HPWaterVolumeSpatialDepthAwareEnabled << "\n";
        out << "HPWaterVolumeSpatialDepthSensitivity: "
            << d.HPWaterVolumeSpatialDepthSensitivity << "\n";
        out << "HPWaterVolumeTemporalNeighborhoodClampStrength: "
            << d.HPWaterVolumeTemporalNeighborhoodClampStrength << "\n";
        out << "HPWaterVolumeHistoryValid: " << d.HPWaterVolumeHistoryValid << "\n";
        out << "HPWaterVolumeHistoryColorTexture: " << d.HPWaterVolumeHistoryColorTexture << "\n";
        out << "HPWaterVolumeHistoryTransmittanceTexture: " << d.HPWaterVolumeHistoryTransmittanceTexture << "\n";
        out << "HPWaterVolumeHistoryDepthTexture: " << d.HPWaterVolumeHistoryDepthTexture << "\n";
        out << "HPWaterVolumeFilterRan: " << d.HPWaterVolumeFilterRan << "\n";
        out << "HPWaterVolumeFilterIterations: " << d.HPWaterVolumeFilterIterations << "\n";
        out << "HPWaterVolumeFilteredColorTexture: " << d.HPWaterVolumeFilteredColorTexture << "\n";
        out << "HPWaterVolumeFilteredTransmittanceTexture: " << d.HPWaterVolumeFilteredTransmittanceTexture << "\n";
        out << "HPWaterVolumeFilteredDepthTexture: " << d.HPWaterVolumeFilteredDepthTexture << "\n";
        out << "HPWaterVolumeUpsampleRan: " << d.HPWaterVolumeUpsampleRan << "\n";
        out << "HPWaterVolumeUpsampledColorTexture: " << d.HPWaterVolumeUpsampledColorTexture << "\n";
        out << "HPWaterVolumeUpsampledTransmittanceTexture: " << d.HPWaterVolumeUpsampledTransmittanceTexture << "\n";
        out << "HPWaterVolumeUpsampledDepthTexture: " << d.HPWaterVolumeUpsampledDepthTexture << "\n";
        out << "HPWaterVolumeUpsampledSize: " << d.HPWaterVolumeUpsampledWidth << "x" << d.HPWaterVolumeUpsampledHeight << "\n";
        out << "HPWaterVolumeUpsampleGatherParityEnabled: "
            << d.HPWaterVolumeUpsampleGatherParityEnabled << "\n";
        out << "HPWaterVolumeUpsampleDepthAwareEnabled: "
            << d.HPWaterVolumeUpsampleDepthAwareEnabled << "\n";
        out << "HPWaterVolumeCompositeFullResolutionEnabled: "
            << d.HPWaterVolumeCompositeFullResolutionEnabled << "\n";
        out << "HPWaterCausticRan: " << d.HPWaterCausticRan << "\n";
        out << "HPWaterCausticValid: " << d.HPWaterCausticValid << "\n";
        out << "HPWaterCausticTexture: " << d.HPWaterCausticTexture << "\n";
        out << "HPWaterCausticComputeRan: " << d.HPWaterCausticComputeRan << "\n";
        out << "HPWaterCausticComputeValid: " << d.HPWaterCausticComputeValid << "\n";
        out << "HPWaterCausticComputeTexture: " << d.HPWaterCausticComputeTexture << "\n";
        out << "HPWaterCausticComputeSize: " << d.HPWaterCausticComputeWidth << "x" << d.HPWaterCausticComputeHeight << "\n";
        out << "HPWaterCausticComputeAtomicEnabled: " << d.HPWaterCausticComputeAtomicEnabled << "\n";
        out << "HPWaterCausticComputeAtomicTexture: " << d.HPWaterCausticComputeAtomicTexture << "\n";
        out << "HPWaterCausticShadowDepthConsumed: " << d.HPWaterCausticShadowDepthConsumed << "\n";
        out << "HPWaterCausticRGBReceiverProjectionEnabled: " << d.HPWaterCausticRGBReceiverProjectionEnabled << "\n";
        out << "HPWaterCausticExponentialLightStepsEnabled: " << d.HPWaterCausticExponentialLightStepsEnabled << "\n";
        out << "HPWaterCausticFrameDitherEnabled: " << d.HPWaterCausticFrameDitherEnabled << "\n";
        out << "HPWaterCausticAtlasReceiverOutputEnabled: " << d.HPWaterCausticAtlasReceiverOutputEnabled << "\n";
        out << "HPWaterCausticCascadeBlendEnabled: " << d.HPWaterCausticCascadeBlendEnabled << "\n";
        out << "HPWaterCausticAtlasEdgeFilterEnabled: " << d.HPWaterCausticAtlasEdgeFilterEnabled << "\n";
        out << "HPWaterCausticGBufferAtlasConsumed: " << d.HPWaterCausticGBufferAtlasConsumed << "\n";
        out << "HPWaterCausticGBufferAtlasDecodeEnabled: " << d.HPWaterCausticGBufferAtlasDecodeEnabled << "\n";
        out << "HPWaterCausticGBufferAtlasDepthAwareEnabled: " << d.HPWaterCausticGBufferAtlasDepthAwareEnabled << "\n";
        out << "HPWaterCausticSpectralWeightingEnabled: " << d.HPWaterCausticSpectralWeightingEnabled << "\n";
        out << "HPWaterCausticFilterRan: " << d.HPWaterCausticFilterRan << "\n";
        out << "HPWaterCausticFilteredValid: " << d.HPWaterCausticFilteredValid << "\n";
        out << "HPWaterCausticFilteredTexture: " << d.HPWaterCausticFilteredTexture << "\n";
        out << "HPWaterCausticFilterIterations: " << d.HPWaterCausticFilterIterations << "\n";
        out << "HPWaterCausticFilterKernelParityEnabled: " << d.HPWaterCausticFilterKernelParityEnabled << "\n";
        out << "HPWaterCausticFilterComputeParityEnabled: " << d.HPWaterCausticFilterComputeParityEnabled << "\n";
        out << "HPWaterCausticFilterLDSHaloEnabled: " << d.HPWaterCausticFilterLDSHaloEnabled << "\n";
        out << "HPWaterCausticStrength: " << d.HPWaterCausticStrength << "\n";
        out << "HPWaterCausticScale: " << d.HPWaterCausticScale << "\n";
        out << "HPWaterCausticDepthFade: " << d.HPWaterCausticDepthFade << "\n";
        out << "HPWaterCausticTransmittanceMaskEnabled: " << d.HPWaterCausticTransmittanceMaskEnabled << "\n";
        out << "HPWaterCausticTransmittanceStrength: " << d.HPWaterCausticTransmittanceStrength << "\n";
        out << "HPWaterCausticLeakReduction: " << d.HPWaterCausticLeakReduction << "\n";
        out << "HPWaterCausticShadowAlphaClipThreshold: " << d.HPWaterCausticShadowAlphaClipThreshold << "\n";
        out << "HPWaterCausticScatterBoost: " << d.HPWaterCausticScatterBoost << "\n";
        out << "HPWaterCausticRGBDispersion: " << d.HPWaterCausticRGBDispersion << "\n";
        out << "HPWaterCausticDispersionStrength: " << d.HPWaterCausticDispersionStrength << "\n";
        out << "HPWaterCausticAtlasRan: " << d.HPWaterCausticAtlasRan << "\n";
        out << "HPWaterCausticAtlasValid: " << d.HPWaterCausticAtlasValid << "\n";
        out << "HPWaterCausticAtlasTexture: " << d.HPWaterCausticAtlasTexture << "\n";
        out << "HPWaterCausticGBufferAtlasTexture: " << d.HPWaterCausticGBufferAtlasTexture << "\n";
        out << "HPWaterCausticAtlasDepthTexture: " << d.HPWaterCausticAtlasDepthTexture << "\n";
        out << "HPWaterCausticAtlasTileResolution: " << d.HPWaterCausticAtlasTileResolution << "\n";
        out << "HPWaterCausticAtlasSize: " << d.HPWaterCausticAtlasWidth << "x" << d.HPWaterCausticAtlasHeight << "\n";
        out << "HPWaterCausticAtlasCascades: " << d.HPWaterCausticAtlasCascades << "\n";
        out << "HPWaterCausticAtlasDrawn: " << d.HPWaterCausticAtlasDrawn << "\n";
        out << "HPWaterCausticAtlasConsumed: " << d.HPWaterCausticAtlasConsumed << "\n";
        out << "HPWaterCausticFilterRadius: " << d.HPWaterCausticFilterRadius << "\n";
        out << "HPWaterCausticFilterDepthSigma: " << d.HPWaterCausticFilterDepthSigma << "\n";
        out << "HPWaterCausticFilterLuminanceWeight: " << d.HPWaterCausticFilterLuminanceWeight << "\n";
        out << "HPWaterCausticVolumeStrength: " << d.HPWaterCausticVolumeStrength << "\n";
        out << "HPWaterFluidDynamicsRan: " << d.HPWaterFluidDynamicsRan << "\n";
        out << "HPWaterFluidDynamicsValid: " << d.HPWaterFluidDynamicsValid << "\n";
        out << "HPWaterFluidComputeRan: " << d.HPWaterFluidComputeRan << "\n";
        out << "HPWaterFluidEdgeAbsorptionParityEnabled: " << d.HPWaterFluidEdgeAbsorptionParityEnabled << "\n";
        out << "HPWaterFluidSourceClampEnabled: " << d.HPWaterFluidSourceClampEnabled << "\n";
        out << "HPWaterFluidMultiSourceEnabled: " << d.HPWaterFluidMultiSourceEnabled << "\n";
        out << "HPWaterFluidSourceCount: " << d.HPWaterFluidSourceCount << "\n";
        out << "HPWaterFluidObjectSourceEnabled: " << d.HPWaterFluidObjectSourceEnabled << "\n";
        out << "HPWaterFluidObjectSourceCount: " << d.HPWaterFluidObjectSourceCount << "\n";
        out << "HPWaterFluidMovingObjectSourceEnabled: " << d.HPWaterFluidMovingObjectSourceEnabled << "\n";
        out << "HPWaterFluidMovingObjectSourceCount: " << d.HPWaterFluidMovingObjectSourceCount << "\n";
        out << "HPWaterFluidWaveEquationParityEnabled: " << d.HPWaterFluidWaveEquationParityEnabled << "\n";
        out << "HPWaterFluidSampleClampParityEnabled: " << d.HPWaterFluidSampleClampParityEnabled << "\n";
        out << "HPWaterFluidStartFrameBakeEnabled: " << d.HPWaterFluidStartFrameBakeEnabled << "\n";
        out << "HPWaterFluidHeightCaptureCacheReused: " << d.HPWaterFluidHeightCaptureCacheReused << "\n";
        out << "HPWaterFluidLayerFilteringParityEnabled: " << d.HPWaterFluidLayerFilteringParityEnabled << "\n";
        out << "HPWaterFluidRenderQueueParityEnabled: " << d.HPWaterFluidRenderQueueParityEnabled << "\n";
        out << "HPWaterFluidSceneOpaqueOnlyCapture: " << d.HPWaterFluidSceneOpaqueOnlyCapture << "\n";
        out << "HPWaterFluidWaterLayerOnlyCapture: " << d.HPWaterFluidWaterLayerOnlyCapture << "\n";
        out << "HPWaterFluidHeightTexture: " << d.HPWaterFluidHeightTexture << "\n";
        out << "HPWaterFluidResolution: " << d.HPWaterFluidResolution << "\n";
        out << "HPWaterFluidWaveSpeed: " << d.HPWaterFluidWaveSpeed << "\n";
        out << "HPWaterFluidDamping: " << d.HPWaterFluidDamping << "\n";
        out << "HPWaterFluidObstacleValid: " << d.HPWaterFluidObstacleValid << "\n";
        out << "HPWaterFluidObstacleTexture: " << d.HPWaterFluidObstacleTexture << "\n";
        out << "HPWaterFluidHeightFieldValid: " << d.HPWaterFluidHeightFieldValid << "\n";
        out << "HPWaterFluidHeightCaptureRan: " << d.HPWaterFluidHeightCaptureRan << "\n";
        out << "HPWaterFluidHeightCaptureValid: " << d.HPWaterFluidHeightCaptureValid << "\n";
        out << "HPWaterFluidCaptureSpaceParityEnabled: " << d.HPWaterFluidCaptureSpaceParityEnabled << "\n";
        out << "HPWaterFluidDisplacedWaterHeightCapture: " << d.HPWaterFluidDisplacedWaterHeightCapture << "\n";
        out << "HPWaterFluidSceneGeometryHeightCapture: " << d.HPWaterFluidSceneGeometryHeightCapture << "\n";
        out << "HPWaterFluidWaterHeightTexture: " << d.HPWaterFluidWaterHeightTexture << "\n";
        out << "HPWaterFluidSceneHeightTexture: " << d.HPWaterFluidSceneHeightTexture << "\n";
        out << "HPWaterFluidCaptureMeshCandidates: " << d.HPWaterFluidCaptureMeshCandidates << "\n";
        out << "HPWaterFluidWaterLayerCandidates: " << d.HPWaterFluidWaterLayerCandidates << "\n";
        out << "HPWaterFluidSceneOpaqueCandidates: " << d.HPWaterFluidSceneOpaqueCandidates << "\n";
        out << "HPWaterFluidWaterCaptureDraws: " << d.HPWaterFluidWaterCaptureDraws << "\n";
        out << "HPWaterFluidSceneCaptureDraws: " << d.HPWaterFluidSceneCaptureDraws << "\n";
        out << "HPWaterFluidWaterLayerMask: " << d.HPWaterFluidWaterLayerMask << "\n";
        out << "HPWaterFluidWaterLayerSkipped: " << d.HPWaterFluidWaterLayerSkipped << "\n";
        out << "HPWaterFluidSceneWaterLayerSkipped: " << d.HPWaterFluidSceneWaterLayerSkipped << "\n";
        out << "HPWaterFluidTransparentSkipped: " << d.HPWaterFluidTransparentSkipped << "\n";
        out << "HPWaterFluidObstacleCount: " << d.HPWaterFluidObstacleCount << "\n";
        out << "HPWaterFluidObstaclePixels: " << d.HPWaterFluidObstaclePixels << "\n";

        auto writeProbe = [&](const char* name, const TextureProbeSummary& p) {
            out << "\n[" << name << "]\n";
            out << "Valid: " << p.Valid << "\n";
            out << "Size: " << p.Width << "x" << p.Height << "\n";
            out << "AverageLuminance: " << std::fixed << std::setprecision(4) << p.AverageLuminance << "\n";
            out << "AverageAlpha: " << std::fixed << std::setprecision(4) << p.AverageAlpha << "\n";
            out << "AverageRGBA: "
                << std::fixed << std::setprecision(4)
                << p.AverageRGBA[0] << ","
                << p.AverageRGBA[1] << ","
                << p.AverageRGBA[2] << ","
                << p.AverageRGBA[3] << "\n";
            out << "NonBlackRatio: " << std::fixed << std::setprecision(4) << p.NonBlackRatio << "\n";
            out << "CenterRGBA: "
                << static_cast<int>(p.Center[0]) << ","
                << static_cast<int>(p.Center[1]) << ","
                << static_cast<int>(p.Center[2]) << ","
                << static_cast<int>(p.Center[3]) << "\n";
            out << "MaxRGBA: "
                << static_cast<int>(p.MaxRGBA[0]) << ","
                << static_cast<int>(p.MaxRGBA[1]) << ","
                << static_cast<int>(p.MaxRGBA[2]) << ","
                << static_cast<int>(p.MaxRGBA[3]) << "\n";
        };

        if (m_Framebuffer) {
            uint32_t sceneTexture = static_cast<uint32_t>(m_Framebuffer->Resolve());
            writeProbe("SceneFramebuffer", ProbeTexture(sceneTexture, m_Framebuffer->GetWidth(), m_Framebuffer->GetHeight()));
            SaveTextureBMP(sceneTexture, m_Framebuffer->GetWidth(), m_Framebuffer->GetHeight(),
                std::filesystem::path(VE_PROJECT_ROOT) / "render_diagnostics_scene.bmp");
        }
        if (dr.IsInitialized() && dr.GetOutputTexture() != 0) {
            writeProbe("DeferredOutput", ProbeTexture(dr.GetOutputTexture(), dr.GetWidth(), dr.GetHeight()));
            SaveTextureBMP(dr.GetOutputTexture(), dr.GetWidth(), dr.GetHeight(),
                std::filesystem::path(VE_PROJECT_ROOT) / "render_diagnostics_deferred.bmp");
        }
        if (dr.IsInitialized() && dr.GetHPWaterCompositeTexture() != 0) {
            writeProbe("HPWaterComposite", ProbeTexture(dr.GetHPWaterCompositeTexture(), dr.GetWidth(), dr.GetHeight()));
            SaveTextureBMP(dr.GetHPWaterCompositeTexture(), dr.GetWidth(), dr.GetHeight(),
                std::filesystem::path(VE_PROJECT_ROOT) / "render_diagnostics_hpwater_composite.bmp");
        }
        if (dr.IsInitialized() && dr.GetHPWaterRefractionDataTexture() != 0) {
            TextureProbeSummary refractionWorldProbe =
                ProbeTexture(dr.GetHPWaterRefractionDataTexture(), dr.GetWidth(), dr.GetHeight());
            TextureFloatProbeSummary refractionWorldFloatProbe =
                ProbeTextureFloat(dr.GetHPWaterRefractionDataTexture(), dr.GetWidth(), dr.GetHeight());
            writeProbe("HPWaterRefractionWorldData", refractionWorldProbe);
            out << "HPWaterRefractionWorldDataAverageWorldPosition: " << std::fixed << std::setprecision(4)
                << refractionWorldFloatProbe.AverageRGBA[0] << ","
                << refractionWorldFloatProbe.AverageRGBA[1] << ","
                << refractionWorldFloatProbe.AverageRGBA[2] << "\n";
            out << "HPWaterRefractionWorldDataAverageRayLength: "
                << refractionWorldFloatProbe.AverageRGBA[3] << "\n";
            out << "HPWaterRefractionWorldDataMaxRayLength: "
                << refractionWorldFloatProbe.MaxRGBA[3] << "\n";
            out << "HPWaterRefractionWorldDataNonZeroWorldRatio: "
                << refractionWorldFloatProbe.NonZeroRGBRatio << "\n";
            out << "HPWaterRefractionWorldDataNonZeroRayRatio: "
                << refractionWorldFloatProbe.NonZeroAlphaRatio << "\n";
            out << "HPWaterRefractionWorldDataAnyRay: "
                << (refractionWorldFloatProbe.MaxRGBA[3] > 0.00001f ? 1 : 0) << "\n";
            SaveTextureBMP(dr.GetHPWaterRefractionDataTexture(), dr.GetWidth(), dr.GetHeight(),
                std::filesystem::path(VE_PROJECT_ROOT) / "render_diagnostics_hpwater_refraction_world_data.bmp");
        }
        if (dr.IsInitialized() && dr.GetHPWaterRefractionMetaTexture() != 0) {
            TextureProbeSummary refractionMetaProbe =
                ProbeTexture(dr.GetHPWaterRefractionMetaTexture(), dr.GetWidth(), dr.GetHeight());
            TextureFloatProbeSummary refractionMetaFloatProbe =
                ProbeTextureFloat(dr.GetHPWaterRefractionMetaTexture(), dr.GetWidth(), dr.GetHeight());
            writeProbe("HPWaterRefractionMeta", refractionMetaProbe);
            out << "HPWaterRefractionMetaAverageUV: " << std::fixed << std::setprecision(4)
                << refractionMetaFloatProbe.AverageRGBA[0] << ","
                << refractionMetaFloatProbe.AverageRGBA[1] << "\n";
            out << "HPWaterRefractionMetaAverageSceneDepth: "
                << refractionMetaFloatProbe.AverageRGBA[2] << "\n";
            out << "HPWaterRefractionMetaAverageNormalizedThickness: "
                << refractionMetaFloatProbe.AverageRGBA[3] << "\n";
            out << "HPWaterRefractionMetaMaxNormalizedThickness: "
                << refractionMetaFloatProbe.MaxRGBA[3] << "\n";
            out << "HPWaterRefractionMetaNonBlackRatio: " << std::fixed << std::setprecision(4)
                << refractionMetaProbe.NonBlackRatio << "\n";
            out << "HPWaterRefractionMetaNonZeroThicknessRatio: "
                << refractionMetaFloatProbe.NonZeroAlphaRatio << "\n";
            out << "HPWaterRefractionMetaAnyThickness: "
                << (refractionMetaFloatProbe.MaxRGBA[3] > 0.00001f ? 1 : 0) << "\n";
            SaveTextureBMP(dr.GetHPWaterRefractionMetaTexture(), dr.GetWidth(), dr.GetHeight(),
                std::filesystem::path(VE_PROJECT_ROOT) / "render_diagnostics_hpwater_refraction_meta.bmp");
        }
        if (dr.IsInitialized() && dr.GetHPWaterSSRDiagnosticsTexture() != 0) {
            TextureProbeSummary ssrDiagnosticsProbe = ProbeTexture(dr.GetHPWaterSSRDiagnosticsTexture(), dr.GetWidth(), dr.GetHeight());
            writeProbe("HPWaterSSRDiagnostics", ssrDiagnosticsProbe);
            const float ssrWeight = ssrDiagnosticsProbe.AverageRGBA[0];
            const float probeWeight = ssrDiagnosticsProbe.AverageRGBA[2];
            const float skyWeight = ssrDiagnosticsProbe.AverageRGBA[3];
            const float hierarchyWeightSum = ssrWeight + probeWeight + skyWeight;
            out << "HPWaterSSRDiagnosticsHierarchyChannelReadbackEnabled: " << ssrDiagnosticsProbe.Valid << "\n";
            out << "HPWaterSSRDiagnosticsAverageConfidence: " << std::fixed << std::setprecision(4) << ssrDiagnosticsProbe.AverageRGBA[0] << "\n";
            out << "HPWaterSSRDiagnosticsAverageHitMask: " << std::fixed << std::setprecision(4) << ssrDiagnosticsProbe.AverageRGBA[1] << "\n";
            out << "HPWaterSSRDiagnosticsAverageProbeHierarchyWeight: " << std::fixed << std::setprecision(4) << ssrDiagnosticsProbe.AverageRGBA[2] << "\n";
            out << "HPWaterSSRDiagnosticsAverageSkyHierarchyWeight: " << std::fixed << std::setprecision(4) << ssrDiagnosticsProbe.AverageRGBA[3] << "\n";
            out << "HPWaterSSRDiagnosticsAverageEnvironmentFallbackWeight: " << std::fixed << std::setprecision(4) << (probeWeight + skyWeight) << "\n";
            out << "HPWaterSSRDiagnosticsAverageHierarchyWeightSum: " << std::fixed << std::setprecision(4) << hierarchyWeightSum << "\n";
            out << "HPWaterSSRDiagnosticsAverageUnallocatedHierarchyWeight: " << std::fixed << std::setprecision(4) << std::max(0.0f, 1.0f - hierarchyWeightSum) << "\n";
            out << "HPWaterSSRDiagnosticsAnySSRHit: " << (ssrDiagnosticsProbe.MaxRGBA[1] > 0 ? 1 : 0) << "\n";
            out << "HPWaterSSRDiagnosticsAnyProbeFallback: " << (ssrDiagnosticsProbe.MaxRGBA[2] > 0 ? 1 : 0) << "\n";
            out << "HPWaterSSRDiagnosticsAnySkyFallback: " << (ssrDiagnosticsProbe.MaxRGBA[3] > 0 ? 1 : 0) << "\n";
            SaveTextureBMP(dr.GetHPWaterSSRDiagnosticsTexture(), dr.GetWidth(), dr.GetHeight(),
                std::filesystem::path(VE_PROJECT_ROOT) / "render_diagnostics_hpwater_ssr_diagnostics.bmp");
        }
        if (dr.IsInitialized() && dr.GetHPWaterSSRLightingTexture() != 0) {
            TextureProbeSummary ssrLightingProbe = ProbeTexture(dr.GetHPWaterSSRLightingTexture(), dr.GetWidth(), dr.GetHeight());
            writeProbe("HPWaterSSRLighting", ssrLightingProbe);
            out << "HPWaterSSRLightingAverageLuminance: " << std::fixed << std::setprecision(4) << ssrLightingProbe.AverageLuminance << "\n";
            out << "HPWaterSSRLightingAverageHierarchyAlpha: " << std::fixed << std::setprecision(4) << ssrLightingProbe.AverageAlpha << "\n";
            out << "HPWaterSSRLightingNonBlackRatio: " << std::fixed << std::setprecision(4) << ssrLightingProbe.NonBlackRatio << "\n";
            out << "HPWaterSSRLightingAnyContribution: " << (ssrLightingProbe.NonBlackRatio > 0.0f ? 1 : 0) << "\n";
            SaveTextureBMP(dr.GetHPWaterSSRLightingTexture(), dr.GetWidth(), dr.GetHeight(),
                std::filesystem::path(VE_PROJECT_ROOT) / "render_diagnostics_hpwater_ssr_lighting.bmp");
        }
        if (dr.IsInitialized() && d.HPWaterDepthPyramidTexture != 0 &&
            d.HPWaterDepthPyramidWidth > 0 && d.HPWaterDepthPyramidHeight > 0) {
            writeProbe("HPWaterDepthPyramidMip0",
                ProbeTexture(d.HPWaterDepthPyramidTexture, d.HPWaterDepthPyramidWidth, d.HPWaterDepthPyramidHeight));
            SaveTextureBMP(d.HPWaterDepthPyramidTexture, d.HPWaterDepthPyramidWidth, d.HPWaterDepthPyramidHeight,
                std::filesystem::path(VE_PROJECT_ROOT) / "render_diagnostics_hpwater_depth_pyramid.bmp");
        }
        if (dr.IsInitialized() && d.HPWaterMaskTexture != 0 &&
            d.HPWaterMaskWidth > 0 && d.HPWaterMaskHeight > 0) {
            writeProbe("HPWaterMask", ProbeTexture(d.HPWaterMaskTexture, d.HPWaterMaskWidth, d.HPWaterMaskHeight));
            SaveTextureBMP(d.HPWaterMaskTexture, d.HPWaterMaskWidth, d.HPWaterMaskHeight,
                std::filesystem::path(VE_PROJECT_ROOT) / "render_diagnostics_hpwater_mask.bmp");
        }
        if (dr.IsInitialized() && d.HPWaterCausticTexture != 0 &&
            d.ViewportWidth > 0 && d.ViewportHeight > 0) {
            writeProbe("HPWaterCaustic", ProbeTexture(d.HPWaterCausticTexture, d.ViewportWidth, d.ViewportHeight));
            SaveTextureBMP(d.HPWaterCausticTexture, d.ViewportWidth, d.ViewportHeight,
                std::filesystem::path(VE_PROJECT_ROOT) / "render_diagnostics_hpwater_caustic.bmp");
        }
        if (dr.IsInitialized() && d.HPWaterCausticComputeTexture != 0 &&
            d.HPWaterCausticComputeValid &&
            d.HPWaterCausticComputeWidth > 0 && d.HPWaterCausticComputeHeight > 0) {
            writeProbe("HPWaterCausticComputeIrradiance",
                ProbeTexture(d.HPWaterCausticComputeTexture,
                             d.HPWaterCausticComputeWidth,
                             d.HPWaterCausticComputeHeight));
            SaveTextureBMP(d.HPWaterCausticComputeTexture,
                d.HPWaterCausticComputeWidth,
                d.HPWaterCausticComputeHeight,
                std::filesystem::path(VE_PROJECT_ROOT) / "render_diagnostics_hpwater_caustic_compute_irradiance.bmp");
        }
        if (dr.IsInitialized() && d.HPWaterCausticFilteredTexture != 0 &&
            d.ViewportWidth > 0 && d.ViewportHeight > 0) {
            writeProbe("HPWaterCausticFiltered",
                ProbeTexture(d.HPWaterCausticFilteredTexture, d.ViewportWidth, d.ViewportHeight));
            SaveTextureBMP(d.HPWaterCausticFilteredTexture, d.ViewportWidth, d.ViewportHeight,
                std::filesystem::path(VE_PROJECT_ROOT) / "render_diagnostics_hpwater_caustic_filtered.bmp");
        }
        if (dr.IsInitialized() && d.HPWaterCausticAtlasTexture != 0 &&
            d.HPWaterCausticAtlasValid &&
            d.HPWaterCausticAtlasWidth > 0 && d.HPWaterCausticAtlasHeight > 0) {
            writeProbe("HPWaterCausticAtlas",
                ProbeTexture(d.HPWaterCausticAtlasTexture,
                             d.HPWaterCausticAtlasWidth,
                             d.HPWaterCausticAtlasHeight));
            SaveTextureBMP(d.HPWaterCausticAtlasTexture,
                d.HPWaterCausticAtlasWidth,
                d.HPWaterCausticAtlasHeight,
                std::filesystem::path(VE_PROJECT_ROOT) / "render_diagnostics_hpwater_caustic_atlas.bmp");
        }
        if (dr.IsInitialized() && d.HPWaterCausticGBufferAtlasTexture != 0 &&
            d.HPWaterCausticAtlasValid &&
            d.HPWaterCausticAtlasWidth > 0 && d.HPWaterCausticAtlasHeight > 0) {
            writeProbe("HPWaterCausticGBufferAtlas",
                ProbeTexture(d.HPWaterCausticGBufferAtlasTexture,
                             d.HPWaterCausticAtlasWidth,
                             d.HPWaterCausticAtlasHeight));
            SaveTextureBMP(d.HPWaterCausticGBufferAtlasTexture,
                d.HPWaterCausticAtlasWidth,
                d.HPWaterCausticAtlasHeight,
                std::filesystem::path(VE_PROJECT_ROOT) / "render_diagnostics_hpwater_caustic_gbuffer_atlas.bmp");
        }
        if (dr.IsInitialized() && d.HPWaterFluidHeightTexture != 0 && d.HPWaterFluidResolution > 0) {
            writeProbe("HPWaterFluidHeight",
                ProbeTexture(d.HPWaterFluidHeightTexture, d.HPWaterFluidResolution, d.HPWaterFluidResolution));
            SaveTextureBMP(d.HPWaterFluidHeightTexture, d.HPWaterFluidResolution, d.HPWaterFluidResolution,
                std::filesystem::path(VE_PROJECT_ROOT) / "render_diagnostics_hpwater_fluid_height.bmp");
        }
        if (dr.IsInitialized() && d.HPWaterSpectrumTexture != 0 && d.HPWaterSpectrumResolution > 0) {
            writeProbe("HPWaterSpectrum",
                ProbeTexture(d.HPWaterSpectrumTexture, d.HPWaterSpectrumResolution, d.HPWaterSpectrumResolution));
            SaveTextureBMP(d.HPWaterSpectrumTexture, d.HPWaterSpectrumResolution, d.HPWaterSpectrumResolution,
                std::filesystem::path(VE_PROJECT_ROOT) / "render_diagnostics_hpwater_spectrum.bmp");
        }
        if (dr.IsInitialized() && d.HPWaterFluidObstacleTexture != 0 && d.HPWaterFluidResolution > 0) {
            writeProbe("HPWaterFluidObstacle",
                ProbeTexture(d.HPWaterFluidObstacleTexture, d.HPWaterFluidResolution, d.HPWaterFluidResolution));
            SaveTextureBMP(d.HPWaterFluidObstacleTexture, d.HPWaterFluidResolution, d.HPWaterFluidResolution,
                std::filesystem::path(VE_PROJECT_ROOT) / "render_diagnostics_hpwater_fluid_obstacle.bmp");
        }
        if (dr.IsInitialized() && d.HPWaterFluidWaterHeightTexture != 0 && d.HPWaterFluidResolution > 0) {
            writeProbe("HPWaterFluidWaterHeight",
                ProbeTexture(d.HPWaterFluidWaterHeightTexture, d.HPWaterFluidResolution, d.HPWaterFluidResolution));
            SaveTextureBMP(d.HPWaterFluidWaterHeightTexture, d.HPWaterFluidResolution, d.HPWaterFluidResolution,
                std::filesystem::path(VE_PROJECT_ROOT) / "render_diagnostics_hpwater_fluid_water_height.bmp");
        }
        if (dr.IsInitialized() && d.HPWaterFluidSceneHeightTexture != 0 && d.HPWaterFluidResolution > 0) {
            writeProbe("HPWaterFluidSceneHeight",
                ProbeTexture(d.HPWaterFluidSceneHeightTexture, d.HPWaterFluidResolution, d.HPWaterFluidResolution));
            SaveTextureBMP(d.HPWaterFluidSceneHeightTexture, d.HPWaterFluidResolution, d.HPWaterFluidResolution,
                std::filesystem::path(VE_PROJECT_ROOT) / "render_diagnostics_hpwater_fluid_scene_height.bmp");
        }
        if (dr.IsInitialized() && d.HPWaterVolumeWidth > 0 && d.HPWaterVolumeHeight > 0) {
            struct HPWaterVolumeProbeTarget {
                const char* Name;
                const char* FileName;
                uint32_t TextureID;
            };
            const HPWaterVolumeProbeTarget volumeTargets[] = {
                { "HPWaterVolumeColor", "render_diagnostics_hpwater_volume_color.bmp", d.HPWaterVolumeColorTexture },
                { "HPWaterVolumeTransmittance", "render_diagnostics_hpwater_volume_transmittance.bmp", d.HPWaterVolumeTransmittanceTexture },
                { "HPWaterVolumeDepth", "render_diagnostics_hpwater_volume_depth.bmp", d.HPWaterVolumeDepthTexture },
                { "HPWaterVolumeMotionVector", "render_diagnostics_hpwater_volume_motion_vector.bmp", d.HPWaterVolumeMotionVectorTexture },
                { "HPWaterVolumeHistoryColor", "render_diagnostics_hpwater_volume_history_color.bmp", d.HPWaterVolumeHistoryColorTexture },
                { "HPWaterVolumeHistoryTransmittance", "render_diagnostics_hpwater_volume_history_transmittance.bmp", d.HPWaterVolumeHistoryTransmittanceTexture },
                { "HPWaterVolumeHistoryDepth", "render_diagnostics_hpwater_volume_history_depth.bmp", d.HPWaterVolumeHistoryDepthTexture },
                { "HPWaterVolumeFilteredColor", "render_diagnostics_hpwater_volume_filtered_color.bmp", d.HPWaterVolumeFilteredColorTexture },
                { "HPWaterVolumeFilteredTransmittance", "render_diagnostics_hpwater_volume_filtered_transmittance.bmp", d.HPWaterVolumeFilteredTransmittanceTexture },
                { "HPWaterVolumeFilteredDepth", "render_diagnostics_hpwater_volume_filtered_depth.bmp", d.HPWaterVolumeFilteredDepthTexture },
            };
            for (const auto& target : volumeTargets) {
                if (target.TextureID == 0)
                    continue;
                writeProbe(target.Name, ProbeTexture(target.TextureID, d.HPWaterVolumeWidth, d.HPWaterVolumeHeight));
                SaveTextureBMP(target.TextureID, d.HPWaterVolumeWidth, d.HPWaterVolumeHeight,
                    std::filesystem::path(VE_PROJECT_ROOT) / target.FileName);
            }
        }
        if (dr.IsInitialized() && d.HPWaterVolumeUpsampledWidth > 0 && d.HPWaterVolumeUpsampledHeight > 0) {
            struct HPWaterUpsampledProbeTarget {
                const char* Name;
                const char* FileName;
                uint32_t TextureID;
            };
            const HPWaterUpsampledProbeTarget upsampledTargets[] = {
                { "HPWaterVolumeUpsampledColor", "render_diagnostics_hpwater_volume_upsampled_color.bmp", d.HPWaterVolumeUpsampledColorTexture },
                { "HPWaterVolumeUpsampledTransmittance", "render_diagnostics_hpwater_volume_upsampled_transmittance.bmp", d.HPWaterVolumeUpsampledTransmittanceTexture },
                { "HPWaterVolumeUpsampledDepth", "render_diagnostics_hpwater_volume_upsampled_depth.bmp", d.HPWaterVolumeUpsampledDepthTexture },
            };
            for (const auto& target : upsampledTargets) {
                if (target.TextureID == 0)
                    continue;
                writeProbe(target.Name, ProbeTexture(target.TextureID, d.HPWaterVolumeUpsampledWidth, d.HPWaterVolumeUpsampledHeight));
                SaveTextureBMP(target.TextureID, d.HPWaterVolumeUpsampledWidth, d.HPWaterVolumeUpsampledHeight,
                    std::filesystem::path(VE_PROJECT_ROOT) / target.FileName);
            }
        }
        if (dr.IsInitialized() && dr.HasHPWaterGBuffer()) {
            struct HPWaterProbeTarget {
                const char* Name;
                const char* FileName;
                uint32_t TextureID;
            };
            const HPWaterProbeTarget hpWaterTargets[] = {
                { "HPWaterNormalRoughness", "render_diagnostics_hpwater_normal_roughness.bmp", dr.GetHPWaterGBufferTexture(0) },
                { "HPWaterScatterThickness", "render_diagnostics_hpwater_scatter_thickness.bmp", dr.GetHPWaterGBufferTexture(1) },
                { "HPWaterAbsorptionFoam", "render_diagnostics_hpwater_absorption_foam.bmp", dr.GetHPWaterGBufferTexture(2) },
            };
            for (const auto& target : hpWaterTargets) {
                if (target.TextureID == 0)
                    continue;
                writeProbe(target.Name, ProbeTexture(target.TextureID, dr.GetWidth(), dr.GetHeight()));
                SaveTextureBMP(target.TextureID, dr.GetWidth(), dr.GetHeight(),
                    std::filesystem::path(VE_PROJECT_ROOT) / target.FileName);
            }
        }
        if (m_PostProcessedTexture != 0 && m_Framebuffer) {
            writeProbe("PostProcessedTexture", ProbeTexture(m_PostProcessedTexture, m_Framebuffer->GetWidth(), m_Framebuffer->GetHeight()));
            SaveTextureBMP(m_PostProcessedTexture, m_Framebuffer->GetWidth(), m_Framebuffer->GetHeight(),
                std::filesystem::path(VE_PROJECT_ROOT) / "render_diagnostics_post.bmp");
        }
    }

    void MaybeAutoExportRenderDiagnostics() {
        if (!m_Scene)
            return;

        const auto& d = m_Scene->GetRenderDiagnostics();

        if (m_RenderDiagnosticsOnce) {
            const bool baseReady = d.FrameIndex > m_RenderDiagnosticsOnceMinFrame &&
                (d.HPWaterEntities > 0 || d.FrameIndex > m_RenderDiagnosticsOnceMaxFrame);
            const bool objectMotionReady =
                !m_RenderDiagnosticsRequireObjectMotion ||
                (d.HPWaterVolumeObjectMotionSourceCount > 0 &&
                 d.HPWaterVolumeObjectMotionFieldEnabled &&
                 d.HPWaterVolumeObjectMotionIDSelectionEnabled &&
                 d.HPWaterVolumeObjectMotionFieldSelected > 0);
            const bool fluidFilteringReady =
                !m_RenderDiagnosticsRequireFluidFiltering ||
                (d.HPWaterFluidHeightCaptureValid &&
                 d.HPWaterFluidMultiSourceEnabled &&
                 d.HPWaterFluidSourceCount > 1 &&
                 d.HPWaterFluidObjectSourceEnabled &&
                 d.HPWaterFluidObjectSourceCount > 0 &&
                 d.HPWaterFluidMovingObjectSourceEnabled &&
                 d.HPWaterFluidMovingObjectSourceCount > 0 &&
                 d.HPWaterFluidLayerFilteringParityEnabled &&
                 d.HPWaterFluidRenderQueueParityEnabled &&
                 d.HPWaterFluidWaterLayerCandidates > 0 &&
                 d.HPWaterFluidSceneOpaqueCandidates > 0 &&
                 d.HPWaterFluidSceneWaterLayerSkipped > 0 &&
                 d.HPWaterFluidTransparentSkipped > 0 &&
                 d.HPWaterFluidWaterCaptureDraws > 0 &&
                 d.HPWaterFluidSceneCaptureDraws > 0);
            const bool ssrReady =
                !m_RenderDiagnosticsRequireSSR ||
                (d.HPWaterSSRReflectionEnabled &&
                 d.HPWaterSSRHierarchyBlendEnabled &&
                 d.HPWaterSSRLightingBufferRan &&
                 d.HPWaterSSRLightingBufferValid &&
                 d.HPWaterSSRLightingRGBPreweighted &&
                 d.HPWaterSSRHitRefinementEnabled &&
                 d.HPWaterSSRRoughnessConeTracingEnabled &&
                 d.HPWaterSSRTemporalResolveEnabled &&
                 d.HPWaterSSRHistoryValid &&
                 d.HPWaterSSRExplicitMotionVectorEnabled &&
                 d.HPWaterSSRMotionVectorHistoryEnabled &&
                 d.HPWaterSSRMotionReprojectionEnabled &&
                 d.HPWaterSSRDisocclusionRejectionEnabled &&
                 d.HPWaterCompositeConsumesSSRLightingBuffer &&
                 d.HPWaterSSRLightingBufferTexture != 0 &&
                 d.HPWaterSSRDiagnosticsValid &&
                 d.HPWaterSSRDiagnosticsTexture != 0 &&
                 d.HPWaterSSRMaxSteps > 0);
            if ((baseReady && objectMotionReady && fluidFilteringReady && ssrReady) || d.FrameIndex > m_RenderDiagnosticsOnceMaxFrame) {
                WriteRenderDiagnosticsFile();
                m_LastAutoRenderDiagnosticFrame = d.FrameIndex;
                glfwSetWindowShouldClose(GetWindow().GetNativeWindow(), true);
            }
            return;
        }

        if (!m_AutoExportRenderDiagnostics)
            return;

        if (d.HPWaterEntities == 0 || d.FrameIndex <= 8)
            return;

        const uint64_t exportInterval = m_LastAutoRenderDiagnosticFrame == 0 ? 30 : 120;
        if (d.FrameIndex - m_LastAutoRenderDiagnosticFrame <= exportInterval)
            return;

        WriteRenderDiagnosticsFile();
        m_LastAutoRenderDiagnosticFrame = d.FrameIndex;
    }

    void DrawRenderDebuggerPanel() {
        if (!m_ShowRenderDebugger) return;
        ImGui::Begin("Render Debugger", &m_ShowRenderDebugger);

        const auto& d = m_Scene->GetRenderDiagnostics();
        ImGui::Text("Frame: %llu", static_cast<unsigned long long>(d.FrameIndex));
        ImGui::Text("Viewport: %ux%u", d.ViewportWidth, d.ViewportHeight);
        ImGui::Separator();
        ImGui::Text("Deferred: init=%d lighting=%d output=%u",
            d.DeferredInitialized ? 1 : 0,
            d.LightingPassRan ? 1 : 0,
            d.DeferredOutputTexture);
        ImGui::Text("Forward transparent: pass=%d queued=%u drawn=%u",
            d.ForwardPassRan ? 1 : 0,
            d.TransparentQueued,
            d.TransparentDrawn);
        ImGui::Text("Opaque submitted: %u", d.OpaqueSubmitted);
        ImGui::Text("Frustum culled: %u", d.FrustumCulled);
        ImGui::Separator();
        ImGui::Text("HPWater: entities=%u mesh=%u queued=%u drawn=%u culled=%u",
            d.HPWaterEntities,
            d.HPWaterWithMesh,
            d.HPWaterQueued,
            d.HPWaterDrawn,
            d.HPWaterCulled);
        ImGui::Text("HPWater pass: gbufferDrawn=%u composite=%d depthMerge=%d normalMerge=%d stencil=%d/0x%02X mask=%d hiz=%d/%u volume=%d temporal=%d history=%d filter=%d/%u upsample=%d compositeTex=%u refractWorld=%u refractMeta=%u",
            d.HPWaterGBufferDrawn,
            d.HPWaterCompositeRan ? 1 : 0,
            d.HPWaterDepthMergedToSceneDepth ? 1 : 0,
            d.HPWaterNormalMergedToSceneGBuffer ? 1 : 0,
            d.HPWaterStencilMarkedInSceneDepth ? 1 : 0,
            d.HPWaterStencilRef,
            d.HPWaterMaskRan ? 1 : 0,
            d.HPWaterDepthPyramidRan ? 1 : 0,
            d.HPWaterDepthPyramidMipCount,
            d.HPWaterVolumeRan ? 1 : 0,
            d.HPWaterVolumeTemporalRan ? 1 : 0,
            d.HPWaterVolumeHistoryValid ? 1 : 0,
            d.HPWaterVolumeFilterRan ? 1 : 0,
            d.HPWaterVolumeFilterIterations,
            d.HPWaterVolumeUpsampleRan ? 1 : 0,
            d.HPWaterCompositeTexture,
            d.HPWaterRefractionDataTexture,
            d.HPWaterRefractionMetaTexture);
        ImGui::Text("HPWater depth pyramid: %ux%u tex=%u",
            d.HPWaterDepthPyramidWidth,
            d.HPWaterDepthPyramidHeight,
            d.HPWaterDepthPyramidTexture);
        ImGui::Text("HPWater volume temporal: clamp=%d motion=%d explicitMV=%d sceneMV=%d objMV=%d field=%d idSelect=%d selected=%u/%u tracked=%u matched=%u moving=%u mvHistory=%d strength=%.2f",
            d.HPWaterVolumeTemporalNeighborhoodClampEnabled ? 1 : 0,
            d.HPWaterVolumeTemporalMotionReprojectionEnabled ? 1 : 0,
            d.HPWaterVolumeExplicitMotionVectorEnabled ? 1 : 0,
            d.HPWaterVolumeSceneMotionVectorEnabled ? 1 : 0,
            d.HPWaterVolumeObjectMotionVectorEnabled ? 1 : 0,
            d.HPWaterVolumeObjectMotionFieldEnabled ? 1 : 0,
            d.HPWaterVolumeObjectMotionIDSelectionEnabled ? 1 : 0,
            d.HPWaterVolumeObjectMotionFieldSelected,
            d.HPWaterVolumeObjectMotionFieldCapacity,
            d.HPWaterVolumeObjectMotionTrackedCount,
            d.HPWaterVolumeObjectMotionMatchedCount,
            d.HPWaterVolumeObjectMotionSourceCount,
            d.HPWaterVolumeMotionVectorHistoryEnabled ? 1 : 0,
            d.HPWaterVolumeTemporalNeighborhoodClampStrength);
        ImGui::Text("HPWater volume object motion offset: %.4f %.4f %.4f",
            d.HPWaterVolumeObjectMotionWorldOffset.x,
            d.HPWaterVolumeObjectMotionWorldOffset.y,
            d.HPWaterVolumeObjectMotionWorldOffset.z);
        ImGui::Text("HPWater volume filter params: blend=%.3f spatial=%d iter=%u mv=%d vel=%.3f depthReject=%d threshold=%.3f depthAware=%d sensitivity=%.1f",
            d.HPWaterVolumeTemporalBlendFactor,
            d.HPWaterVolumeSpatialFilterEnabled ? 1 : 0,
            d.HPWaterVolumeSpatialFilterIterations,
            d.HPWaterVolumeMotionVectorsEnabled ? 1 : 0,
            d.HPWaterVolumeMotionVectorVelocityScale,
            d.HPWaterVolumeTemporalDepthRejectionEnabled ? 1 : 0,
            d.HPWaterVolumeTemporalDepthThreshold,
            d.HPWaterVolumeSpatialDepthAwareEnabled ? 1 : 0,
            d.HPWaterVolumeSpatialDepthSensitivity);
        ImGui::Text("HPWater volume phase/shadows: sceneScatter=%d albedoPhase=%d phaseG=%d sampling=%d params=%d maxCross=%d dynamicShadow=%d maxCrossDist=%.2f softness=%.2f minFilter=%.2f blockers=%u filters=%u",
            d.HPWaterVolumeSceneInScatteringEnabled ? 1 : 0,
            d.HPWaterVolumeAlbedoPhaseBlendEnabled ? 1 : 0,
            d.HPWaterVolumePhaseGEnabled ? 1 : 0,
            d.HPWaterVolumeShadowSamplingEnabled ? 1 : 0,
            d.HPWaterVolumeShadowParamsEnabled ? 1 : 0,
            d.HPWaterVolumeMaxCrossDistanceEnabled ? 1 : 0,
            d.HPWaterVolumeDynamicShadowDistanceEnabled ? 1 : 0,
            d.HPWaterVolumeMaxCrossDistance,
            d.HPWaterVolumeShadowSoftness,
            d.HPWaterVolumeShadowMinFilterSize,
            d.HPWaterVolumeShadowBlockerSamples,
            d.HPWaterVolumeShadowFilterSamples);
        ImGui::Text("HPWater mask: %ux%u tex=%u",
            d.HPWaterMaskWidth,
            d.HPWaterMaskHeight,
            d.HPWaterMaskTexture);
        ImGui::Text("HPWater refraction: ndc=%d strength=%.3f dispersion=%.3f maxCross=%.2f thickness=%.2f samples=%u jitter=%d",
            d.HPWaterRefractionNDCMarchEnabled ? 1 : 0,
            d.HPWaterRefractionStrength,
            d.HPWaterWaterDispersionStrength,
            d.HPWaterMaxRefractionCrossDistance,
            d.HPWaterRefractionThicknessOffset,
            d.HPWaterRefractionSampleCount,
            d.HPWaterRefractionJitterEnabled ? 1 : 0);
        ImGui::Text("HPWater BSDF: env=%.3f indirect=%.3f macro=%.3f thinSSS=%.3f backlit=%.3f forward=%.3f blur=%.3f multi=%.2f phaseG=%.2f fgd=%.3f energy=%.3f exitF=%d/%.5f lut=%d/%u",
            d.HPWaterEnvironmentReflectionIntensity,
            d.HPWaterIndirectLightStrength,
            d.HPWaterMacroScatterStrength,
            d.HPWaterThinSSSStrength,
            d.HPWaterBacklitTransmissionStrength,
            d.HPWaterForwardScatterStrength,
            d.HPWaterForwardScatterBlurDensity,
            d.HPWaterMultiScatterScale,
            d.HPWaterPhaseG,
            d.HPWaterSpecularFGDStrength,
            d.HPWaterGGXEnergyCompensation,
            d.HPWaterExitFresnelEnabled ? 1 : 0,
            d.HPWaterExitFresnelF0,
            d.HPWaterPreintegratedFGDLUTValid ? 1 : 0,
            d.HPWaterPreintegratedFGDLUTResolution);
        ImGui::Text("HPWater light loop: valid=%d surfaceShadow=%d cascadeDither=%d punctual=%d areaApprox=%d areaRect=%d areaLTC=%d/%u/%u ltcHDRP=%d ltcSample=%d ltcHDRPUV=%d ltcCosTheta=%d ltcMatrix=%d ltcPoly=%d ltcHorizonClip=%d point=%u/%u spot=%u/%u area=%u/%u cap=%u/%u/%u layerFilter=%d areaLayerFilter=%d influenceSort=%d layerSkip=%u areaLayerSkip=%u capSkip=%u areaCapSkip=%u volumePunctual=%d volumeArea=%d volumeAreaRect=%d volumeAreaPoly=%d volumeHorizonClip=%d vPoint=%u vSpot=%u vArea=%u indirectScatter=%d bsdfWeights=%d punctualBody=%d specOcc=%d skyRefl=%.3f indirect=%.3f dir=%.3f",
            d.HPWaterLightLoopInputsValid ? 1 : 0,
            d.HPWaterSurfaceShadowSamplingEnabled ? 1 : 0,
            d.HPWaterShadowCascadeDitherEnabled ? 1 : 0,
            d.HPWaterPunctualLightLoopEnabled ? 1 : 0,
            d.HPWaterAreaLightApproximationEnabled ? 1 : 0,
            d.HPWaterAreaLightRectangleSamplingEnabled ? 1 : 0,
            d.HPWaterAreaLightLTCLUTValid ? 1 : 0,
            d.HPWaterAreaLightLTCLUTResolution,
            d.HPWaterAreaLightLTCLUTLayers,
            d.HPWaterAreaLightLTCHDRPTableEnabled ? 1 : 0,
            d.HPWaterAreaLightLTCSamplingEnabled ? 1 : 0,
            d.HPWaterAreaLightLTCHDRPUVEnabled ? 1 : 0,
            d.HPWaterAreaLightLTCCosThetaParamEnabled ? 1 : 0,
            d.HPWaterAreaLightLTCMatrixCoefficientsEnabled ? 1 : 0,
            d.HPWaterAreaLightLTCPolygonIntegrationEnabled ? 1 : 0,
            d.HPWaterAreaLightLTCHorizonClippingEnabled ? 1 : 0,
            d.HPWaterPointLightCount,
            d.HPWaterPunctualPointLightCandidates,
            d.HPWaterSpotLightCount,
            d.HPWaterPunctualSpotLightCandidates,
            d.HPWaterAreaLightCount,
            d.HPWaterAreaLightCandidates,
            d.HPWaterPunctualPointLightCapacity,
            d.HPWaterPunctualSpotLightCapacity,
            d.HPWaterAreaLightCapacity,
            d.HPWaterPunctualLightLayerFilteringEnabled ? 1 : 0,
            d.HPWaterAreaLightLayerFilteringEnabled ? 1 : 0,
            d.HPWaterPunctualLightInfluenceSortingEnabled ? 1 : 0,
            d.HPWaterPunctualLightsLayerSkipped,
            d.HPWaterAreaLightsLayerSkipped,
            d.HPWaterPunctualLightsCapacitySkipped,
            d.HPWaterAreaLightsCapacitySkipped,
            d.HPWaterVolumePunctualLightLoopEnabled ? 1 : 0,
            d.HPWaterVolumeAreaLightLoopEnabled ? 1 : 0,
            d.HPWaterVolumeAreaLightRectangleSamplingEnabled ? 1 : 0,
            d.HPWaterVolumeAreaLightLTCPolygonIntegrationEnabled ? 1 : 0,
            d.HPWaterVolumeAreaLightLTCHorizonClippingEnabled ? 1 : 0,
            d.HPWaterVolumePointLightCount,
            d.HPWaterVolumeSpotLightCount,
            d.HPWaterVolumeAreaLightCount,
            d.HPWaterIndirectScatterIntegrationEnabled ? 1 : 0,
            d.HPWaterBSDFComponentWeightingEnabled ? 1 : 0,
            d.HPWaterPunctualBodyComponentWeightingEnabled ? 1 : 0,
            d.HPWaterSpecularSelfOcclusionEnabled ? 1 : 0,
            d.HPWaterSkyReflectionIntensity,
            d.HPWaterIndirectDiffuseIntensity,
            d.HPWaterDirectionalLightIntensity);
        ImGui::Text("HPWater light selection: bounds=%d center=(%.2f, %.2f, %.2f) radius=%.2f topScore p/s/a=%.3f/%.3f/%.3f selectedScore p/s/a=%.3f/%.3f/%.3f capSkip p/s=%u/%u",
            d.HPWaterLightSelectionBoundsValid ? 1 : 0,
            d.HPWaterLightSelectionCenter.x,
            d.HPWaterLightSelectionCenter.y,
            d.HPWaterLightSelectionCenter.z,
            d.HPWaterLightSelectionRadius,
            d.HPWaterPointLightTopInfluenceScore,
            d.HPWaterSpotLightTopInfluenceScore,
            d.HPWaterAreaLightTopInfluenceScore,
            d.HPWaterPointLightSelectedInfluenceSum,
            d.HPWaterSpotLightSelectedInfluenceSum,
            d.HPWaterAreaLightSelectedInfluenceSum,
            d.HPWaterPunctualPointLightsCapacitySkipped,
            d.HPWaterPunctualSpotLightsCapacitySkipped);
        ImGui::Text("HPWater spectrum: ocean=%d normal=%d compute=%d/%d consumed=%d tex=%u res=%u amp=%.3f normalStrength=%.3f wind=%.2f spread=%.2f swell=%.2f shortFade=%.2f windModel=%d freq=%d phillips=%d jonswap=%d ifft=%d passes=%d",
            d.HPWaterSpectralOceanEnabled ? 1 : 0,
            d.HPWaterSpectralNormalParityEnabled ? 1 : 0,
            d.HPWaterSpectrumComputeRan ? 1 : 0,
            d.HPWaterSpectrumComputeValid ? 1 : 0,
            d.HPWaterSpectrumTextureConsumed ? 1 : 0,
            d.HPWaterSpectrumTexture,
            d.HPWaterSpectrumResolution,
            d.HPWaterSpectrumAmplitude,
            d.HPWaterSpectrumNormalStrength,
            d.HPWaterSpectrumWindSpeed,
            d.HPWaterSpectrumDirectionalSpread,
            d.HPWaterSpectrumSwell,
            d.HPWaterSpectrumShortWaveFade,
            d.HPWaterSpectrumWindModelEnabled ? 1 : 0,
            d.HPWaterSpectrumFrequencyDomainEnabled ? 1 : 0,
            d.HPWaterSpectrumPhillipsEnabled ? 1 : 0,
            d.HPWaterSpectrumJonswapEnabled ? 1 : 0,
            d.HPWaterSpectrumIFFTEnabled ? 1 : 0,
            d.HPWaterSpectrumButterflyPasses);
        ImGui::Text("HPWater environment: skyTex=%d (%u) probe=%d box=%d dominant=%d exact=%d multiBounce=%d (%u/%u) intensity=%.3f blend=%.3f influence=%.3f hierarchy=%.3f",
            d.HPWaterSkyTextureReflectionBound ? 1 : 0,
            d.HPWaterSkyTexture,
            d.HPWaterReflectionProbeBound ? 1 : 0,
            d.HPWaterReflectionProbeBoxProjectionEnabled ? 1 : 0,
            d.HPWaterEnvSpecularDominantDirEnabled ? 1 : 0,
            d.HPWaterEnvSpecularDominantDirExactFormulaEnabled ? 1 : 0,
            d.HPWaterEnvSpecularMultiBounceEnabled ? 1 : 0,
            d.HPWaterReflectionProbeTexture,
            d.HPWaterReflectionProbeSecondaryTexture,
            d.HPWaterReflectionProbeIntensity,
            d.HPWaterReflectionProbeBlend,
            d.HPWaterReflectionProbeInfluenceWeight,
            d.HPWaterReflectionProbeHierarchyWeight);
        ImGui::Text("HPWater SSR hierarchy: enabled=%d blend=%d ssrBuf=%d/%d pre=%d refine=%d cone=%d temporal=%d history=%d mv=%d mvHistory=%d reproj=%d disocc=%d consume=%d (%u) diag=%d (%u) steps=%u step=%.3f thickness=%.3f maxDist=%.1f",
            d.HPWaterSSRReflectionEnabled ? 1 : 0,
            d.HPWaterSSRHierarchyBlendEnabled ? 1 : 0,
            d.HPWaterSSRLightingBufferRan ? 1 : 0,
            d.HPWaterSSRLightingBufferValid ? 1 : 0,
            d.HPWaterSSRLightingRGBPreweighted ? 1 : 0,
            d.HPWaterSSRHitRefinementEnabled ? 1 : 0,
            d.HPWaterSSRRoughnessConeTracingEnabled ? 1 : 0,
            d.HPWaterSSRTemporalResolveEnabled ? 1 : 0,
            d.HPWaterSSRHistoryValid ? 1 : 0,
            d.HPWaterSSRExplicitMotionVectorEnabled ? 1 : 0,
            d.HPWaterSSRMotionVectorHistoryEnabled ? 1 : 0,
            d.HPWaterSSRMotionReprojectionEnabled ? 1 : 0,
            d.HPWaterSSRDisocclusionRejectionEnabled ? 1 : 0,
            d.HPWaterCompositeConsumesSSRLightingBuffer ? 1 : 0,
            d.HPWaterSSRLightingBufferTexture,
            d.HPWaterSSRDiagnosticsValid ? 1 : 0,
            d.HPWaterSSRDiagnosticsTexture,
            d.HPWaterSSRMaxSteps,
            d.HPWaterSSRStepSize,
            d.HPWaterSSRThickness,
            d.HPWaterSSRMaxDistance);
        ImGui::Text("HPWater forward scatter mips: enabled=%d count=%u",
            d.HPWaterForwardScatterMipEnabled ? 1 : 0,
            d.HPWaterForwardScatterMipCount);
        ImGui::Text("HPWater volume raw: %ux%u color=%u trans=%u depth=%u",
            d.HPWaterVolumeWidth,
            d.HPWaterVolumeHeight,
            d.HPWaterVolumeColorTexture,
            d.HPWaterVolumeTransmittanceTexture,
            d.HPWaterVolumeDepthTexture);
        ImGui::Text("HPWater volume filtered: color=%u trans=%u depth=%u",
            d.HPWaterVolumeFilteredColorTexture,
            d.HPWaterVolumeFilteredTransmittanceTexture,
            d.HPWaterVolumeFilteredDepthTexture);
        ImGui::Text("HPWater volume upsampled: %ux%u color=%u trans=%u depth=%u",
            d.HPWaterVolumeUpsampledWidth,
            d.HPWaterVolumeUpsampledHeight,
            d.HPWaterVolumeUpsampledColorTexture,
            d.HPWaterVolumeUpsampledTransmittanceTexture,
            d.HPWaterVolumeUpsampledDepthTexture);
        ImGui::Text("HPWater caustic: ran=%d valid=%d tex=%u compute=%d/%d atomic=%d tex=%u %ux%u exp=%d dither=%d atlasRecv=%d blend=%d edge=%d spec=%d filtered=%d/%u kernel=%d computeFilter=%d lds=%d tex=%u strength=%.3f scale=%.2f depthFade=%.2f trans=%.2f leak=%.2f scatter=%.2f rgb=%d dispersion=%.3f filterRadius=%.2f lum=%.2f volume=%.3f",
            d.HPWaterCausticRan ? 1 : 0,
            d.HPWaterCausticValid ? 1 : 0,
            d.HPWaterCausticTexture,
            d.HPWaterCausticComputeRan ? 1 : 0,
            d.HPWaterCausticComputeValid ? 1 : 0,
            d.HPWaterCausticComputeAtomicEnabled ? 1 : 0,
            d.HPWaterCausticComputeTexture,
            d.HPWaterCausticComputeWidth,
            d.HPWaterCausticComputeHeight,
            d.HPWaterCausticExponentialLightStepsEnabled ? 1 : 0,
            d.HPWaterCausticFrameDitherEnabled ? 1 : 0,
            d.HPWaterCausticAtlasReceiverOutputEnabled ? 1 : 0,
            d.HPWaterCausticCascadeBlendEnabled ? 1 : 0,
            d.HPWaterCausticAtlasEdgeFilterEnabled ? 1 : 0,
            d.HPWaterCausticSpectralWeightingEnabled ? 1 : 0,
            d.HPWaterCausticFilteredValid ? 1 : 0,
            d.HPWaterCausticFilterIterations,
            d.HPWaterCausticFilterKernelParityEnabled ? 1 : 0,
            d.HPWaterCausticFilterComputeParityEnabled ? 1 : 0,
            d.HPWaterCausticFilterLDSHaloEnabled ? 1 : 0,
            d.HPWaterCausticFilteredTexture,
            d.HPWaterCausticStrength,
            d.HPWaterCausticScale,
            d.HPWaterCausticDepthFade,
            d.HPWaterCausticTransmittanceStrength,
            d.HPWaterCausticLeakReduction,
            d.HPWaterCausticScatterBoost,
            d.HPWaterCausticRGBDispersion ? 1 : 0,
            d.HPWaterCausticDispersionStrength,
            d.HPWaterCausticFilterRadius,
            d.HPWaterCausticFilterLuminanceWeight,
            d.HPWaterCausticVolumeStrength);
        ImGui::Text("HPWater caustic tuning: transmittance=%.2f leak=%.2f shadowClip=%.2f scatter=%.2f rgb=%d dispersion=%.2f",
            d.HPWaterCausticTransmittanceStrength,
            d.HPWaterCausticLeakReduction,
            d.HPWaterCausticShadowAlphaClipThreshold,
            d.HPWaterCausticScatterBoost,
            d.HPWaterCausticRGBDispersion ? 1 : 0,
            d.HPWaterCausticDispersionStrength);
        ImGui::Text("HPWater caustic atlas: ran=%d valid=%d consumed=%d drawn=%u tile=%u size=%ux%u cascades=%u tex=%u depth=%u",
            d.HPWaterCausticAtlasRan ? 1 : 0,
            d.HPWaterCausticAtlasValid ? 1 : 0,
            d.HPWaterCausticAtlasConsumed ? 1 : 0,
            d.HPWaterCausticAtlasDrawn,
            d.HPWaterCausticAtlasTileResolution,
            d.HPWaterCausticAtlasWidth,
            d.HPWaterCausticAtlasHeight,
            d.HPWaterCausticAtlasCascades,
            d.HPWaterCausticAtlasTexture,
            d.HPWaterCausticAtlasDepthTexture);
        ImGui::Text("HPWater fluid: ran=%d valid=%d compute=%d multiSrc=%d sources=%u objectSources=%u movingWake=%u res=%u height=%u speed=%.3f damping=%.3f",
            d.HPWaterFluidDynamicsRan ? 1 : 0,
            d.HPWaterFluidDynamicsValid ? 1 : 0,
            d.HPWaterFluidComputeRan ? 1 : 0,
            d.HPWaterFluidMultiSourceEnabled ? 1 : 0,
            d.HPWaterFluidSourceCount,
            d.HPWaterFluidObjectSourceCount,
            d.HPWaterFluidMovingObjectSourceCount,
            d.HPWaterFluidResolution,
            d.HPWaterFluidHeightTexture,
            d.HPWaterFluidWaveSpeed,
            d.HPWaterFluidDamping);
        ImGui::Text("HPWater fluid obstacles: valid=%d texture=%u count=%u pixels=%u",
            d.HPWaterFluidObstacleValid ? 1 : 0,
            d.HPWaterFluidObstacleTexture,
            d.HPWaterFluidObstacleCount,
            d.HPWaterFluidObstaclePixels);
        ImGui::Text("HPWater fluid height fields: valid=%d capture=%d/%d displaced=%d sceneGeo=%d water=%u scene=%u draws=%u/%u",
            d.HPWaterFluidHeightFieldValid ? 1 : 0,
            d.HPWaterFluidHeightCaptureRan ? 1 : 0,
            d.HPWaterFluidHeightCaptureValid ? 1 : 0,
            d.HPWaterFluidDisplacedWaterHeightCapture ? 1 : 0,
            d.HPWaterFluidSceneGeometryHeightCapture ? 1 : 0,
            d.HPWaterFluidWaterHeightTexture,
            d.HPWaterFluidSceneHeightTexture,
            d.HPWaterFluidWaterCaptureDraws,
            d.HPWaterFluidSceneCaptureDraws);
        ImGui::Text("HPWater fluid capture parity: queue=%d sceneOpaque=%d waterLayer=%d mask=0x%08X candidates=%u water=%u scene=%u skipped water=%u sceneLayer=%u transparent=%u",
            d.HPWaterFluidRenderQueueParityEnabled ? 1 : 0,
            d.HPWaterFluidSceneOpaqueOnlyCapture ? 1 : 0,
            d.HPWaterFluidWaterLayerOnlyCapture ? 1 : 0,
            d.HPWaterFluidWaterLayerMask,
            d.HPWaterFluidCaptureMeshCandidates,
            d.HPWaterFluidWaterLayerCandidates,
            d.HPWaterFluidSceneOpaqueCandidates,
            d.HPWaterFluidWaterLayerSkipped,
            d.HPWaterFluidSceneWaterLayerSkipped,
            d.HPWaterFluidTransparentSkipped);
        ImGui::Text("HPWater GBuffer: init=%d attachments=%u rt0=%u rt1=%u rt2=%u depth=%u",
            d.HPWaterGBufferInitialized ? 1 : 0,
            d.HPWaterGBufferAttachmentCount,
            d.HPWaterGBuffer0,
            d.HPWaterGBuffer1,
            d.HPWaterGBuffer2,
            d.HPWaterGBufferDepth);

        ImGui::Checkbox("Auto export when HPWater exists", &m_AutoExportRenderDiagnostics);

        if (ImGui::Button("Export Render Diagnostics")) {
            WriteRenderDiagnosticsFile();
            m_LastAutoRenderDiagnosticFrame = d.FrameIndex;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("writes render_diagnostics.txt");

        ImGui::End();
    }

    void DrawPipelineSettingsPanel() {
        if (!m_ShowPipelineSettings) return;
        ImGui::Begin("Render Pipeline", &m_ShowPipelineSettings);

        auto& ps = m_Scene->GetPipelineSettings();

        if (ImGui::CollapsingHeader("Render Pipeline", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("Pipeline: Deferred");
            ImGui::TextDisabled("G-Buffer MRT: Position, Normal, Albedo, Emission");
            ImGui::TextDisabled("HPWater MRT: Normal, Scatter, Absorption");
            ImGui::TextDisabled("Transparent objects use forward pass");

            const char* debugViews[] = { "None", "Position", "Normals", "Albedo",
                "Metallic", "Roughness", "AO", "Emission", "Depth",
                "HPWater Normal/Roughness", "HPWater Scatter/Thickness", "HPWater Absorption/Foam" };
            ImGui::Combo("G-Buffer Debug", &ps.GBufferDebugView, debugViews, 12);
        }

        if (ImGui::CollapsingHeader("HDR / Tone Mapping", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Enable HDR", &ps.HDREnabled);
            if (ps.HDREnabled) {
                const char* tmModes[] = { "Reinhard", "ACES Filmic", "Uncharted 2" };
                ImGui::Combo("Tone Map Operator", &ps.ToneMapMode, tmModes, 3);
                ImGui::SliderFloat("Exposure##HDR", &ps.Exposure, 0.1f, 10.0f, "%.2f");
                if (ImGui::Button("Reset Exposure")) ps.Exposure = 1.0f;
            }
        }

        if (ImGui::CollapsingHeader("Sky", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Enable Sky", &ps.SkyEnabled);

            if (ps.SkyEnabled) {
                ImGui::ColorEdit3("Top Color", ps.SkyTopColor.data());
                ImGui::ColorEdit3("Bottom Color", ps.SkyBottomColor.data());

                // Sky Texture — Object Field with drag-drop
                {
                    bool hasTex = !ps.SkyTexturePath.empty();
                    std::string displayName = hasTex
                        ? std::filesystem::path(ps.SkyTexturePath).filename().generic_string()
                        : "None (Texture2D)";

                    ImGui::PushID("SkyTexField");
                    ImGui::Text("Sky Texture");
                    ImGui::SameLine(100.0f);

                    float availW = ImGui::GetContentRegionAvail().x;
                    float clearBtnW = ImGui::GetFrameHeight();
                    float fieldW = availW - clearBtnW - ImGui::GetStyle().ItemSpacing.x;

                    ImVec2 pos = ImGui::GetCursorScreenPos();
                    ImVec2 size(fieldW, ImGui::GetFrameHeight());

                    ImU32 bgCol = IM_COL32(40, 40, 40, 255);
                    ImU32 borderCol = IM_COL32(80, 80, 80, 255);
                    bool hovered = ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + size.x, pos.y + size.y));
                    if (hovered) borderCol = IM_COL32(120, 160, 255, 255);

                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bgCol, 3.0f);
                    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), borderCol, 3.0f);

                    // Icon (blue tint for textures)
                    ImU32 iconCol = hasTex ? IM_COL32(100, 180, 255, 255) : IM_COL32(160, 160, 160, 255);
                    float iconSize = size.y * 0.6f;
                    float iconPad = (size.y - iconSize) * 0.5f;
                    ImVec2 iconMin(pos.x + 4.0f + iconPad, pos.y + iconPad);
                    ImVec2 iconMax(iconMin.x + iconSize, iconMin.y + iconSize);
                    dl->AddRectFilled(iconMin, iconMax, iconCol, 2.0f);

                    // Text
                    float textX = iconMax.x + 6.0f;
                    float textY = pos.y + (size.y - ImGui::GetTextLineHeight()) * 0.5f;
                    dl->AddText(ImVec2(textX, textY), IM_COL32(200, 200, 200, 255), displayName.c_str());

                    ImGui::InvisibleButton("##skytexfield", size);

                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_TEXTURE")) {
                            std::string path(static_cast<const char*>(payload->Data));
                            ps.SkyTexturePath = path;
                            ps.SkyTexture = VE::Texture2D::Create(path);
                        }
                        ImGui::EndDragDropTarget();
                    }

                    ImGui::SameLine();
                    if (ImGui::Button("x##SkyTexClear", ImVec2(clearBtnW, 0))) {
                        ps.SkyTexturePath.clear();
                        ps.SkyTexture.reset();
                    }

                    ImGui::PopID();
                }
            }
        }

        if (ImGui::CollapsingHeader("Indirect Lighting", ImGuiTreeNodeFlags_DefaultOpen)) {
            bool indirectDirty = false;
            indirectDirty |= ImGui::Checkbox("Enable Indirect Lighting", &ps.IndirectLightingEnabled);
            if (ps.IndirectLightingEnabled) {
                indirectDirty |= ImGui::SliderFloat("Diffuse Intensity##Indirect", &ps.IndirectDiffuseIntensity, 0.0f, 2.0f, "%.2f");
                indirectDirty |= ImGui::SliderFloat("Sky Reflection Intensity", &ps.SkyReflectionIntensity, 0.0f, 2.0f, "%.2f");
                indirectDirty |= ImGui::ColorEdit3("Tint##Indirect", ps.IndirectTint.data());
            }
            if (indirectDirty)
                MarkDirty();
        }

        if (ImGui::CollapsingHeader("Cascaded Shadows", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Enable Shadows", &ps.ShadowsEnabled);
            if (ps.ShadowsEnabled) {
                const char* resOptions[] = { "512", "1024", "2048", "4096" };
                int resIdx = 2;
                if (ps.ShadowResolution <= 512) resIdx = 0;
                else if (ps.ShadowResolution <= 1024) resIdx = 1;
                else if (ps.ShadowResolution <= 2048) resIdx = 2;
                else resIdx = 3;
                if (ImGui::Combo("Resolution##Shadows", &resIdx, resOptions, 4)) {
                    const int resValues[] = { 512, 1024, 2048, 4096 };
                    ps.ShadowResolution = resValues[resIdx];
                }
                ImGui::SliderFloat("Max Distance", &ps.ShadowMaxDistance, 10.0f, 1000.0f, "%.0f");
                ImGui::SliderFloat("Split Lambda", &ps.ShadowSplitLambda, 0.0f, 1.0f, "%.2f");
                ImGui::SliderFloat("Depth Bias", &ps.ShadowDepthBias, 0.0f, 0.02f, "%.5f");
                ImGui::SliderFloat("Normal Bias", &ps.ShadowNormalBias, 0.0f, 0.1f, "%.4f");
                const char* pcfOptions[] = { "Hard", "PCF 3x3", "PCF 5x5" };
                ImGui::Combo("PCF Quality", &ps.ShadowPCFQuality, pcfOptions, 3);
                ImGui::SliderFloat("Blend Width", &ps.ShadowCascadeBlendWidth, 0.0f, 0.3f, "%.2f");
            }
        }

        if (ImGui::CollapsingHeader("Occlusion Culling", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Enable Occlusion Culling", &ps.OcclusionCullingEnabled);
            if (ps.OcclusionCullingEnabled) {
                ImGui::TextDisabled("GPU occlusion queries (previous-frame temporal)");
                auto& renderStats = VE::RenderCommand::GetStats();
                ImGui::Text("Occluded: %u", renderStats.OccludedObjects);
            }
        }

        if (ImGui::CollapsingHeader("Bloom", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Enable Bloom", &ps.BloomEnabled);
            if (ps.BloomEnabled) {
                ImGui::SliderFloat("Threshold", &ps.BloomThreshold, 0.0f, 2.0f, "%.2f");
                ImGui::SliderFloat("Bloom Intensity", &ps.BloomIntensity, 0.0f, 5.0f, "%.2f");
                ImGui::SliderInt("Blur Passes", &ps.BloomIterations, 1, 10);
            }
        }

        if (ImGui::CollapsingHeader("Vignette", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Enable Vignette", &ps.VignetteEnabled);
            if (ps.VignetteEnabled) {
                ImGui::SliderFloat("Vignette Intensity", &ps.VignetteIntensity, 0.0f, 2.0f, "%.2f");
                ImGui::SliderFloat("Smoothness", &ps.VignetteSmoothness, 0.01f, 1.0f, "%.2f");
            }
        }

        if (ImGui::CollapsingHeader("Color Adjustments", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Enable Color Adjustments", &ps.ColorAdjustEnabled);
            if (ps.ColorAdjustEnabled) {
                ImGui::SliderFloat("Exposure", &ps.ColorExposure, -5.0f, 5.0f, "%.2f EV");
                ImGui::SliderFloat("Contrast", &ps.ColorContrast, -1.0f, 1.0f, "%.2f");
                ImGui::SliderFloat("Saturation", &ps.ColorSaturation, -1.0f, 1.0f, "%.2f");
                ImGui::ColorEdit3("Color Filter", ps.ColorFilter.data());
                ImGui::SliderFloat("Gamma", &ps.ColorGamma, 0.1f, 5.0f, "%.2f");
            }
        }

        if (ImGui::CollapsingHeader("Shadows / Midtones / Highlights")) {
            ImGui::Checkbox("Enable SMH", &ps.SMHEnabled);
            if (ps.SMHEnabled) {
                ImGui::ColorEdit3("Shadows##SMH", ps.SMH_Shadows.data(),
                    ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
                ImGui::ColorEdit3("Midtones##SMH", ps.SMH_Midtones.data(),
                    ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
                ImGui::ColorEdit3("Highlights##SMH", ps.SMH_Highlights.data(),
                    ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
                ImGui::Separator();
                ImGui::Text("Tonal Ranges");
                ImGui::SliderFloat("Shadow End", &ps.SMH_ShadowEnd, 0.0f, 0.5f, "%.2f");
                ImGui::SliderFloat("Highlight Start", &ps.SMH_HighlightStart, 0.3f, 1.0f, "%.2f");
            }
        }

        if (ImGui::CollapsingHeader("Color Curves")) {
            ImGui::Checkbox("Enable Curves", &ps.CurvesEnabled);
            if (ps.CurvesEnabled) {
                DrawCurveEditor("Master", ps.CurvesMaster, IM_COL32(200, 200, 200, 255));
                DrawCurveEditor("Red",    ps.CurvesRed,    IM_COL32(255,  80,  80, 255));
                DrawCurveEditor("Green",  ps.CurvesGreen,  IM_COL32( 80, 255,  80, 255));
                DrawCurveEditor("Blue",   ps.CurvesBlue,   IM_COL32( 80,  80, 255, 255));
            }
        }

        if (ImGui::CollapsingHeader("Tonemapping")) {
            ImGui::Checkbox("Enable Tonemapping", &ps.TonemapEnabled);
            if (ps.TonemapEnabled) {
                const char* modes[] = { "None", "Reinhard", "ACES Filmic", "Uncharted 2" };
                ImGui::Combo("Operator", &ps.TonemapMode, modes, 4);
            }
        }

        if (ImGui::CollapsingHeader("Fog", ImGuiTreeNodeFlags_DefaultOpen)) {
            bool fogDirty = false;
            fogDirty |= ImGui::Checkbox("Enable Fog", &ps.FogEnabled);
            if (ps.FogEnabled) {
                const char* fogModes[] = { "Linear", "Exponential", "Exponential Squared" };
                fogDirty |= ImGui::Combo("Fog Mode", &ps.FogMode, fogModes, 3);
                fogDirty |= ImGui::ColorEdit3("Fog Color", ps.FogColor.data());
                if (ps.FogMode == 0) { // Linear
                    fogDirty |= ImGui::DragFloat("Start Distance", &ps.FogStart, 1.0f, 0.0f, 10000.0f);
                    fogDirty |= ImGui::DragFloat("End Distance", &ps.FogEnd, 1.0f, 0.0f, 10000.0f);
                } else {
                    fogDirty |= ImGui::SliderFloat("Density", &ps.FogDensity, 0.001f, 0.5f, "%.4f");
                }
                fogDirty |= ImGui::SliderFloat("Height Falloff", &ps.FogHeightFalloff, 0.0f, 1.0f, "%.3f");
                fogDirty |= ImGui::SliderFloat("Max Opacity", &ps.FogMaxOpacity, 0.0f, 1.0f, "%.2f");
            }
            if (fogDirty)
                MarkDirty();
        }

        if (ImGui::CollapsingHeader("Volumetric Fog")) {
            bool volFogDirty = false;
            volFogDirty |= ImGui::Checkbox("Enable Volumetric Fog", &ps.VolFogEnabled);
            if (ps.VolFogEnabled) {
                volFogDirty |= ImGui::ColorEdit3("Fog Color##Vol", ps.VolFogColor.data());
                volFogDirty |= ImGui::SliderFloat("Density##Vol", &ps.VolFogDensity, 0.001f, 0.2f, "%.4f");
                volFogDirty |= ImGui::SliderFloat("Scattering (g)", &ps.VolFogScattering, 0.0f, 0.99f, "%.2f");
                ImGui::TextDisabled("0 = isotropic, 1 = forward (god rays)");
                volFogDirty |= ImGui::SliderFloat("Light Intensity##Vol", &ps.VolFogLightIntensity, 0.0f, 5.0f, "%.2f");
                volFogDirty |= ImGui::SliderInt("March Steps", &ps.VolFogSteps, 8, 128);
                volFogDirty |= ImGui::DragFloat("Max Distance##Vol", &ps.VolFogMaxDistance, 1.0f, 1.0f, 1000.0f);
                volFogDirty |= ImGui::SliderFloat("Height Falloff##Vol", &ps.VolFogHeightFalloff, 0.0f, 0.5f, "%.3f");
                volFogDirty |= ImGui::DragFloat("Base Height", &ps.VolFogBaseHeight, 0.5f, -100.0f, 100.0f);
            }
            if (volFogDirty)
                MarkDirty();
        }

        if (ImGui::CollapsingHeader("SSAO", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Enable SSAO", &ps.SSAOEnabled);
            if (ps.SSAOEnabled) {
                ImGui::SliderFloat("Radius##SSAO", &ps.SSAORadius, 0.01f, 3.0f, "%.2f");
                ImGui::SliderFloat("Bias##SSAO", &ps.SSAOBias, 0.001f, 0.1f, "%.3f");
                ImGui::SliderFloat("Intensity##SSAO", &ps.SSAOIntensity, 0.0f, 3.0f, "%.2f");
                ImGui::SliderInt("Kernel Size##SSAO", &ps.SSAOKernelSize, 4, 64);
            }
        }

        if (ImGui::CollapsingHeader("SSR (Reflections)", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Enable SSR", &ps.SSREnabled);
            if (ps.SSREnabled) {
                ImGui::SliderInt("Max Steps##SSR", &ps.SSRMaxSteps, 8, 128);
                ImGui::SliderFloat("Step Size##SSR", &ps.SSRStepSize, 0.005f, 0.2f, "%.3f");
                ImGui::SliderFloat("Thickness##SSR", &ps.SSRThickness, 0.01f, 1.0f, "%.3f");
                ImGui::SliderFloat("Max Distance##SSR", &ps.SSRMaxDistance, 5.0f, 200.0f, "%.1f");
            }
        }

        if (ImGui::CollapsingHeader("Motion Blur")) {
            ImGui::Checkbox("Enable Motion Blur", &ps.MotionBlurEnabled);
            if (ps.MotionBlurEnabled) {
                ImGui::SliderFloat("Blur Strength", &ps.MotionBlurStrength, 0.0f, 1.0f, "%.2f");
                ImGui::SliderInt("Blur Samples", &ps.MotionBlurSamples, 4, 16);
                ImGui::TextDisabled("Camera-based motion blur from depth reprojection");
            }
        }

        if (ImGui::CollapsingHeader("Depth of Field")) {
            ImGui::Checkbox("Enable DoF", &ps.DoFEnabled);
            if (ps.DoFEnabled) {
                ImGui::DragFloat("Focus Distance", &ps.DoFFocusDistance, 0.5f, 0.1f, 500.0f, "%.1f");
                ImGui::SliderFloat("Focus Range", &ps.DoFFocusRange, 0.1f, 50.0f, "%.1f");
                ImGui::TextDisabled("Transition zone around focus distance");
                ImGui::SliderFloat("Max Blur", &ps.DoFMaxBlur, 1.0f, 10.0f, "%.1f");
                ImGui::SliderFloat("Aperture Size", &ps.DoFApertureSize, 0.01f, 0.5f, "%.3f");
                ImGui::TextDisabled("Larger aperture = stronger blur");
            }
        }

        if (ImGui::CollapsingHeader("Anti-Aliasing", ImGuiTreeNodeFlags_DefaultOpen)) {
            const char* aaModes[] = { "None", "MSAA 2x", "MSAA 4x", "MSAA 8x", "FXAA", "TAA" };
            ImGui::Combo("AA Mode", &ps.AAMode, aaModes, 6);

            if (ps.AAMode >= 1 && ps.AAMode <= 3) {
                ImGui::TextDisabled("Hardware multisample anti-aliasing");
            }
            if (ps.AAMode == 4) { // FXAA
                ImGui::SliderFloat("Edge Threshold", &ps.FXAAEdgeThreshold, 0.01f, 0.5f, "%.4f");
                ImGui::SliderFloat("Edge Threshold Min", &ps.FXAAEdgeThresholdMin, 0.001f, 0.1f, "%.4f");
                ImGui::SliderFloat("Subpixel Quality", &ps.FXAASubpixelQuality, 0.0f, 1.0f, "%.2f");
            }
            if (ps.AAMode == 5) { // TAA
                ImGui::SliderFloat("Blend Factor", &ps.TAABlendFactor, 0.01f, 0.5f, "%.2f");
                ImGui::TextDisabled("Lower = more temporal smoothing");
            }
        }

        ImGui::End();
    }

    // ── Project Settings Panel ──────────────────────────────────────────

    void DrawProjectSettingsPanel() {
        if (!m_ShowProjectSettings) return;
        ImGui::Begin("Project Settings", &m_ShowProjectSettings);

        // Left-side category list + right-side content
        static int selectedCategory = 0;
        const char* categories[] = { "Tags and Layers", "Auto-Save" };
        constexpr int categoryCount = 2;

        ImGui::BeginChild("##categories", ImVec2(150, 0), true);
        for (int i = 0; i < categoryCount; i++) {
            if (ImGui::Selectable(categories[i], selectedCategory == i))
                selectedCategory = i;
        }
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("##content", ImVec2(0, 0), true);

        if (selectedCategory == 1) {
            // ── Auto-Save Settings ──
            ImGui::Text("Auto-Save");
            ImGui::Separator();
            ImGui::Checkbox("Enabled", &m_AutoSaveEnabled);

            ImGui::SliderFloat("Interval (seconds)", &m_AutoSaveInterval, 30.0f, 600.0f, "%.0f");
            ImGui::TextDisabled("Auto-saves to ProjectSettings/AutoSave.vscene");

            ImGui::Spacing();
            if (m_SceneDirty)
                ImGui::TextColored(ImVec4(1, 1, 0, 1), "Scene has unsaved changes");
            else
                ImGui::TextColored(ImVec4(0, 1, 0, 1), "Scene is up to date");

            if (m_AutoSaveEnabled) {
                float remaining = m_AutoSaveInterval - m_AutoSaveTimer;
                if (m_SceneDirty && remaining > 0)
                    ImGui::Text("Next auto-save in: %.0f seconds", remaining);
            }
        }

        if (selectedCategory == 0) {
            // ── Tags ──
            if (ImGui::CollapsingHeader("Tags", ImGuiTreeNodeFlags_DefaultOpen)) {
                for (int i = 0; i < (int)m_CustomTags.size(); i++) {
                    ImGui::PushID(i);
                    char buf[128];
                    strncpy(buf, m_CustomTags[i].c_str(), sizeof(buf) - 1);
                    buf[sizeof(buf) - 1] = '\0';

                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 30);
                    if (ImGui::InputText("##tag", buf, sizeof(buf)))
                        m_CustomTags[i] = buf;

                    // Don't allow removing built-in tags (first 7)
                    if (i >= VE::kDefaultTagCount) {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("-")) {
                            m_CustomTags.erase(m_CustomTags.begin() + i);
                            ImGui::PopID();
                            break;
                        }
                    }
                    ImGui::PopID();
                }
                if (ImGui::Button("+ Add Tag")) {
                    m_CustomTags.push_back("New Tag");
                }
            }

            ImGui::Spacing();

            // ── Layers ──
            if (ImGui::CollapsingHeader("Layers", ImGuiTreeNodeFlags_DefaultOpen)) {
                for (int i = 0; i < 32; i++) {
                    ImGui::PushID(i + 1000);
                    char label[16];
                    snprintf(label, sizeof(label), "Layer %d", i);

                    char buf[64];
                    strncpy(buf, m_LayerNames[i].c_str(), sizeof(buf) - 1);
                    buf[sizeof(buf) - 1] = '\0';

                    // Built-in layers (0-5) are read-only
                    if (i <= 5 && m_LayerNames[i].length() > 0) {
                        ImGui::Text("%s", label);
                        ImGui::SameLine(80);
                        ImGui::TextDisabled("%s (Built-in)", m_LayerNames[i].c_str());
                    } else {
                        ImGui::Text("%s", label);
                        ImGui::SameLine(80);
                        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                        if (ImGui::InputText("##layer", buf, sizeof(buf)))
                            m_LayerNames[i] = buf;
                    }
                    ImGui::PopID();
                }
            }
        }

        ImGui::EndChild();
        ImGui::End();
    }

    // Navigate Content Browser to the asset and select it
    void PingAsset(const std::string& absolutePath) {
        std::string relPath = m_AssetDatabase.GetRelativePath(absolutePath);
        if (relPath.empty()) return;

        // Navigate browser to the parent directory
        auto slashPos = relPath.rfind('/');
        m_BrowserCurrentDir = (slashPos != std::string::npos) ? relPath.substr(0, slashPos) : "";

        // Select the asset and show it in inspector
        m_SelectedAssetPath = relPath;
        ClearSelection();

        // Ensure Content Browser is visible
        m_ShowContentBrowser = true;
    }

    // ── Game View Panel ──────────────────────────────────────────────

    void DrawGameViewPanel() {
        if (!m_ShowGameView) return;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("Game", &m_ShowGameView);

        ImVec2 viewportSize = ImGui::GetContentRegionAvail();
        if (viewportSize.x > 0 && viewportSize.y > 0 && m_GameFramebuffer) {
            uint32_t w = static_cast<uint32_t>(viewportSize.x);
            uint32_t h = static_cast<uint32_t>(viewportSize.y);
            if (m_GameFramebuffer->GetWidth() != w || m_GameFramebuffer->GetHeight() != h)
                m_GameFramebuffer->Resize(w, h);
        }

        // Check if there's a main camera (Tag == "MainCamera", or sole camera)
        bool hasCam = false;
        {
            auto camCheck = m_Scene->GetAllEntitiesWith<VE::TagComponent, VE::TransformComponent, VE::CameraComponent>();
            int cnt = 0;
            for (auto e : camCheck) {
                cnt++;
                auto& tag = camCheck.get<VE::TagComponent>(e);
                if (tag.GameObjectTag == "MainCamera") { hasCam = true; break; }
            }
            if (cnt == 1) hasCam = true;
        }

        if (hasCam && m_GameFramebuffer) {
            uint64_t texID = (m_GamePostProcessedTexture != 0)
                ? static_cast<uint64_t>(m_GamePostProcessedTexture)
                : m_GameFramebuffer->GetColorAttachmentID();
            if (texID != 0)
                ImGui::Image((ImTextureID)texID, viewportSize, ImVec2(0, 1), ImVec2(1, 0));
        } else {
            ImVec2 center(ImGui::GetWindowWidth() * 0.5f, ImGui::GetWindowHeight() * 0.5f);
            const char* msg = "No camera in scene";
            ImVec2 textSize = ImGui::CalcTextSize(msg);
            ImGui::SetCursorPos(ImVec2(center.x - textSize.x * 0.5f, center.y - textSize.y * 0.5f));
            ImGui::TextDisabled("%s", msg);
        }

        ImGui::End();
        ImGui::PopStyleVar();
    }

    // ── Content Browser Panel ─────────────────────────────────────────

    void DrawContentBrowserPanel() {
        if (!m_ShowContentBrowser) return;
        ImGui::Begin("Content Browser", &m_ShowContentBrowser);

        const float fontSize = ImGui::GetFontSize();
        const float toolbarHeight = fontSize + ImGui::GetStyle().FramePadding.y * 2.0f;

        // Top toolbar: Unity-style breadcrumb on the left, search/tools on the right.
        {
            if (ImGui::Button("+", ImVec2(toolbarHeight, toolbarHeight))) {
                ImGui::OpenPopup("CreateAssetPopup");
            }
            if (ImGui::BeginPopup("CreateAssetPopup")) {
                if (ImGui::MenuItem("Folder")) {
                    m_AssetDatabase.CreateFolder(m_BrowserCurrentDir, "New Folder");
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Material")) {
                    CreateAssetFile(m_BrowserCurrentDir, "New Material", ".vmat");
                }
                if (ImGui::MenuItem("Shader")) {
                    CreateAssetFile(m_BrowserCurrentDir, "New Shader", ".shader");
                }
                if (ImGui::MenuItem("C++ Script")) {
                    CreateAssetFile(m_BrowserCurrentDir, "NewScript", ".cpp");
                }
                ImGui::EndPopup();
            }

            ImGui::SameLine();
            if (ImGui::Button("Assets"))
                m_BrowserCurrentDir.clear();

            if (!m_BrowserCurrentDir.empty()) {
                // Show path segments as clickable buttons
                std::string accumulated;
                std::istringstream ss(m_BrowserCurrentDir);
                std::string segment;
                while (std::getline(ss, segment, '/')) {
                    ImGui::SameLine();
                    ImGui::Text("/");
                    ImGui::SameLine();
                    accumulated = accumulated.empty() ? segment : accumulated + "/" + segment;
                    std::string id = segment + "##" + accumulated;
                    if (ImGui::Button(id.c_str()))
                        m_BrowserCurrentDir = accumulated;
                }
            }

            const float sliderWidth = 110.0f;
            const float searchWidth = 220.0f;
            const float refreshWidth = 72.0f;
            float rightStart = ImGui::GetWindowWidth() - searchWidth - sliderWidth - refreshWidth - 42.0f;
            if (rightStart > ImGui::GetCursorPosX())
                ImGui::SameLine(rightStart);
            else
                ImGui::SameLine();

            ImGui::SetNextItemWidth(searchWidth);
            ImGui::InputTextWithHint("##assetSearch", "Search", m_ContentSearch, sizeof(m_ContentSearch));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(sliderWidth);
            ImGui::SliderFloat("##iconSize", &m_ContentIconSize, 72.0f, 144.0f, "%.0f");
            ImGui::SameLine();
            if (ImGui::Button("Refresh")) {
                m_AssetDatabase.Refresh();
                ScanAndRegisterAssets();
            }
        }

        ImGui::Separator();

        // Two-column layout: folder tree | contents
        float treeWidth = 260.0f;

        // Left pane: folder tree
        ImGui::BeginChild("FolderTree", ImVec2(treeWidth, 0), true);
        ImGuiTreeNodeFlags rootFlags = ImGuiTreeNodeFlags_OpenOnArrow |
            ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (m_BrowserCurrentDir.empty())
            rootFlags |= ImGuiTreeNodeFlags_Selected;
        bool rootOpen = ImGui::TreeNodeEx("Assets", rootFlags);
        if (ImGui::IsItemClicked())
            m_BrowserCurrentDir.clear();
        if (rootOpen) {
        DrawFolderTree("");
            ImGui::TreePop();
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // Right pane: contents grid
        ImGui::BeginChild("Contents", ImVec2(0, 0), true);
        m_ContentItemPositions.clear();

        // Right-click context menu (on empty space)
        if (ImGui::BeginPopupContextWindow("BrowserPopup", ImGuiPopupFlags_NoOpenOverItems | ImGuiPopupFlags_MouseButtonRight)) {
            if (ImGui::MenuItem("New Folder")) {
                m_AssetDatabase.CreateFolder(m_BrowserCurrentDir, "New Folder");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("New Material")) {
                CreateAssetFile(m_BrowserCurrentDir, "New Material", ".vmat");
            }
            if (ImGui::MenuItem("New Shader")) {
                CreateAssetFile(m_BrowserCurrentDir, "New Shader", ".shader");
            }
            if (ImGui::MenuItem("New Script")) {
                CreateAssetFile(m_BrowserCurrentDir, "NewScript", ".cpp");
            }
            ImGui::EndPopup();
        }

        auto contents = m_AssetDatabase.GetDirectoryContents(m_BrowserCurrentDir);
        std::vector<std::string> filteredContents;
        filteredContents.reserve(contents.size());
        std::string search = m_ContentSearch;
        std::transform(search.begin(), search.end(), search.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (auto& relPath : contents) {
            if (search.empty()) {
                filteredContents.push_back(relPath);
                continue;
            }

            std::string filename = std::filesystem::path(relPath).filename().generic_string();
            std::string haystack = filename;
            std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (haystack.find(search) != std::string::npos)
                filteredContents.push_back(relPath);
        }

        float padding = 18.0f;
        float iconSize = m_ContentIconSize;
        float labelHeight = fontSize * 2.0f + 10.0f;
        float cellSize = iconSize + 72.0f;
        float tileHeight = iconSize + labelHeight + 20.0f;
        float panelWidth = ImGui::GetContentRegionAvail().x;
        int columns = std::max(1, static_cast<int>(panelWidth / (cellSize + padding)));

        ImGui::Columns(columns, nullptr, false);

        auto truncateText = [](const std::string& text, float maxWidth) -> std::string {
            if (ImGui::CalcTextSize(text.c_str()).x <= maxWidth)
                return text;

            const std::string ellipsis = "...";
            if (ImGui::CalcTextSize(ellipsis.c_str()).x > maxWidth)
                return "";

            size_t len = text.size();
            while (len > 0) {
                std::string candidate = text.substr(0, len) + ellipsis;
                if (ImGui::CalcTextSize(candidate.c_str()).x <= maxWidth)
                    return candidate;
                --len;
            }
            return ellipsis;
        };

        auto splitLabelLines = [&](const std::string& text, float maxWidth) {
            std::array<std::string, 2> lines = { "", "" };
            if (text.empty())
                return lines;

            if (ImGui::CalcTextSize(text.c_str()).x <= maxWidth) {
                lines[0] = text;
                return lines;
            }

            size_t split = 0;
            for (size_t i = 1; i < text.size(); ++i) {
                std::string first = text.substr(0, i);
                if (ImGui::CalcTextSize(first.c_str()).x > maxWidth)
                    break;
                split = i;
            }

            if (split == 0) {
                lines[0] = truncateText(text, maxWidth);
                return lines;
            }

            lines[0] = text.substr(0, split);
            lines[1] = truncateText(text.substr(split), maxWidth);
            return lines;
        };

        auto drawFileIcon = [&](ImDrawList* dl, ImVec2 iconMin, ImVec2 iconMax,
                                VE::AssetType type, const std::string& filename,
                                uint64_t texID) {
            const float w = iconMax.x - iconMin.x;
            const float h = iconMax.y - iconMin.y;
            const ImU32 docColor = IM_COL32(230, 230, 230, 255);
            const ImU32 foldColor = IM_COL32(188, 188, 188, 255);
            const ImU32 outlineColor = IM_COL32(35, 35, 35, 120);

            if (type == VE::AssetType::Texture2D && texID != 0) {
                dl->AddRectFilled(iconMin, iconMax, IM_COL32(70, 70, 70, 255), 3.0f);
                ImVec2 imgMin(iconMin.x + 4.0f, iconMin.y + 4.0f);
                ImVec2 imgMax(iconMax.x - 4.0f, iconMax.y - 4.0f);
                dl->AddImage((ImTextureID)texID, imgMin, imgMax);
                return;
            }

            if (type == VE::AssetType::Folder) {
                ImVec2 tabMin(iconMin.x + w * 0.12f, iconMin.y + h * 0.22f);
                ImVec2 tabMax(iconMin.x + w * 0.48f, iconMin.y + h * 0.39f);
                ImVec2 bodyMin(iconMin.x + w * 0.10f, iconMin.y + h * 0.33f);
                ImVec2 bodyMax(iconMin.x + w * 0.90f, iconMin.y + h * 0.78f);
                dl->AddRectFilled(tabMin, tabMax, IM_COL32(198, 198, 198, 255), 4.0f);
                dl->AddRectFilled(bodyMin, bodyMax, IM_COL32(188, 188, 188, 255), 5.0f);
                dl->AddRect(bodyMin, bodyMax, outlineColor, 5.0f);
                return;
            }

            ImVec2 docMin(iconMin.x + w * 0.18f, iconMin.y + h * 0.08f);
            ImVec2 docMax(iconMin.x + w * 0.82f, iconMin.y + h * 0.86f);
            ImVec2 foldA(docMax.x - w * 0.18f, docMin.y);
            ImVec2 foldB(docMax.x, docMin.y + h * 0.18f);
            dl->AddRectFilled(docMin, docMax, docColor, 5.0f);
            dl->AddTriangleFilled(foldA, ImVec2(docMax.x, docMin.y), foldB, foldColor);
            dl->AddRect(docMin, docMax, outlineColor, 5.0f);

            const char* label = "FILE";
            ImU32 labelColor = IM_COL32(90, 90, 90, 255);
            if (type == VE::AssetType::Scene) {
                label = "SCN";
                labelColor = IM_COL32(82, 132, 68, 255);
            } else if (type == VE::AssetType::Mesh) {
                std::string ext = std::filesystem::path(filename).extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                label = (ext == ".fbx") ? "FBX" : (ext == ".obj") ? "OBJ" : "GLTF";
                labelColor = IM_COL32(110, 84, 150, 255);
            } else if (type == VE::AssetType::Shader) {
                label = "SHD";
                labelColor = IM_COL32(132, 86, 138, 255);
            } else if (type == VE::AssetType::MaterialAsset) {
                label = "MAT";
                labelColor = IM_COL32(184, 126, 52, 255);
            } else if (type == VE::AssetType::Audio) {
                label = "SND";
                labelColor = IM_COL32(72, 142, 112, 255);
            } else if (type == VE::AssetType::Script) {
                label = "C++";
                labelColor = IM_COL32(50, 118, 74, 255);
            }

            ImVec2 textSize = ImGui::CalcTextSize(label);
            ImVec2 textPos(iconMin.x + (w - textSize.x) * 0.5f, iconMin.y + h * 0.42f);
            dl->AddText(textPos, labelColor, label);
        };

        for (auto& relPath : filteredContents) {
            auto* meta = m_AssetDatabase.GetMetaByPath(relPath);
            if (!meta) continue;

            std::string filename = std::filesystem::path(relPath).filename().generic_string();
            std::string displayName = (meta->Type != VE::AssetType::Folder)
                ? std::filesystem::path(filename).stem().string() : filename;

            ImGui::PushID(relPath.c_str());

            ImVec2 tileStart = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##item", ImVec2(cellSize, tileHeight));
            bool hovered = ImGui::IsItemHovered();
            ImVec2 tileMin = ImGui::GetItemRectMin();
            ImVec2 tileMax = ImGui::GetItemRectMax();
            ImVec2 iconMin(tileMin.x + (cellSize - iconSize) * 0.5f, tileMin.y + 10.0f);
            ImVec2 iconMax(iconMin.x + iconSize, iconMin.y + iconSize);

            ImDrawList* dl = ImGui::GetWindowDrawList();
            uint64_t texID = 0;
            if (meta->Type == VE::AssetType::Texture2D) {
                std::string absPath = m_AssetDatabase.GetAbsolutePath(relPath);
                texID = m_ThumbnailCache.GetThumbnail(absPath);
            }
            drawFileIcon(dl, iconMin, iconMax, meta->Type, filename, texID);

            // Track item center for box selection
            ImVec2 itemCenter = { (tileMin.x + tileMax.x) * 0.5f, (tileMin.y + tileMax.y) * 0.5f };
            m_ContentItemPositions.push_back({ relPath, itemCenter });

            // Selection highlight (multi-select aware)
            bool isAssetSelected = m_SelectedAssets.count(relPath) > 0;
            if (isAssetSelected)
                dl->AddRect(tileMin, tileMax, IM_COL32(66, 133, 220, 255), 4.0f, 0, 2.0f);
            else if (hovered)
                dl->AddRect(tileMin, tileMax, IM_COL32(120, 120, 120, 170), 4.0f, 0, 1.5f);

            // Click selection with Ctrl/Shift modifiers
            if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !ImGui::GetDragDropPayload()) {
                ImGuiIO& io = ImGui::GetIO();
                if (io.KeyCtrl) {
                    // Ctrl+Click: toggle selection
                    if (m_SelectedAssets.count(relPath))
                        m_SelectedAssets.erase(relPath);
                    else
                        m_SelectedAssets.insert(relPath);
                    m_SelectedAssetPath = relPath;
                } else if (io.KeyShift && !m_SelectedAssetPath.empty()) {
                    // Shift+Click: range select from last selected to this
                    bool inRange = false;
                    for (auto& rp : filteredContents) {
                        if (rp == m_SelectedAssetPath || rp == relPath) {
                            m_SelectedAssets.insert(rp);
                            if (inRange) break;
                            inRange = true;
                        } else if (inRange) {
                            m_SelectedAssets.insert(rp);
                        }
                    }
                    m_SelectedAssetPath = relPath;
                } else {
                    // Normal click: single select
                    m_SelectedAssets.clear();
                    m_SelectedAssets.insert(relPath);
                    m_SelectedAssetPath = relPath;
                }
                ClearSelection(); // deselect entity
            }

            // Double-click actions
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (meta->Type == VE::AssetType::Folder) {
                    m_BrowserCurrentDir = relPath;
                } else if (meta->Type == VE::AssetType::Scene) {
                    std::string absPath = m_AssetDatabase.GetAbsolutePath(relPath);
                    m_Scene = std::make_shared<VE::Scene>();
                    ClearSelection();
                    VE::SceneSerializer serializer(m_Scene);
                    if (serializer.Deserialize(absPath))
                        m_CurrentScenePath = absPath;
                } else if (meta->Type == VE::AssetType::MaterialAsset) {
                    // Open Material Editor on double-click
                    std::string absPath = m_AssetDatabase.GetAbsolutePath(relPath);
                    m_InspectedMaterial = VE::Material::Load(absPath);
                    m_InspectedMaterialPath = absPath;
                    if (m_InspectedMaterial) {
                        VE::MaterialLibrary::Register(m_InspectedMaterial);
                        m_InspectedMaterial->PopulateFromShader();
                    }
                    m_ShowMaterialEditor = true;
                } else if (meta->Type == VE::AssetType::Mesh) {
                    std::string absPath = m_AssetDatabase.GetAbsolutePath(relPath);
                    auto meshAsset = VE::MeshImporter::GetOrLoad(absPath);
                    if (meshAsset && meshAsset->VAO) {
                        auto entity = m_Scene->CreateEntity(meshAsset->GetName());
                        auto& mr = entity.AddComponent<VE::MeshRendererComponent>();
                        mr.Mesh = meshAsset->VAO;
                        mr.Mat = VE::MaterialLibrary::Get("Lit");
                        mr.MeshSourcePath = absPath;
                    }
                } else if (meta->Type == VE::AssetType::Script || meta->Type == VE::AssetType::Shader) {
                    OpenInExternalEditor(m_AssetDatabase.GetAbsolutePath(relPath));
                }
            }

            // Drag-drop source for mesh files
            if (meta->Type == VE::AssetType::Mesh) {
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                    std::string absPath = m_AssetDatabase.GetAbsolutePath(relPath);
                    ImGui::SetDragDropPayload("ASSET_MESH", absPath.c_str(), absPath.size() + 1);
                    ImGui::Text("%s", filename.c_str());
                    ImGui::EndDragDropSource();
                }
            }

            // Drag-drop source for audio files
            if (meta->Type == VE::AssetType::Audio) {
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                    std::string absPath = m_AssetDatabase.GetAbsolutePath(relPath);
                    ImGui::SetDragDropPayload("ASSET_AUDIO", absPath.c_str(), absPath.size() + 1);
                    ImGui::Text("%s", filename.c_str());
                    ImGui::EndDragDropSource();
                }
            }

            // Drag-drop source for texture files
            if (meta->Type == VE::AssetType::Texture2D) {
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                    std::string absPath = m_AssetDatabase.GetAbsolutePath(relPath);
                    ImGui::SetDragDropPayload("ASSET_TEXTURE", absPath.c_str(), absPath.size() + 1);
                    ImGui::Text("%s", filename.c_str());
                    ImGui::EndDragDropSource();
                }
            }

            // Drag-drop source for material files
            if (meta->Type == VE::AssetType::MaterialAsset) {
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                    std::string absPath = m_AssetDatabase.GetAbsolutePath(relPath);
                    ImGui::SetDragDropPayload("ASSET_MATERIAL", absPath.c_str(), absPath.size() + 1);
                    ImGui::Text("%s", filename.c_str());
                    ImGui::EndDragDropSource();
                }
            }

            // Right-click context menu on item
            if (ImGui::BeginPopupContextItem()) {
                // If right-clicked item is not in selection, select it alone
                if (m_SelectedAssets.find(relPath) == m_SelectedAssets.end()) {
                    m_SelectedAssets.clear();
                    m_SelectedAssets.insert(relPath);
                    m_SelectedAssetPath = relPath;
                }

                int selCount = static_cast<int>(m_SelectedAssets.size());
                char label[64];
                snprintf(label, sizeof(label), selCount > 1 ? "Delete %d items" : "Delete", selCount);

                if (ImGui::MenuItem(label)) {
                    // Copy set since deletion invalidates iterators
                    auto toDelete = m_SelectedAssets;
                    m_SelectedAssets.clear();
                    m_SelectedAssetPath.clear();
                    for (auto& p : toDelete)
                        m_AssetDatabase.DeleteAsset(p);
                    ImGui::EndPopup();
                    ImGui::PopID();
                    ImGui::Columns(1);
                    ImGui::EndChild();
                    ImGui::End();
                    return; // collection invalidated
                }
                ImGui::EndPopup();
            }

            const float labelWidth = cellSize - 12.0f;
            auto labelLines = splitLabelLines(displayName, labelWidth);
            bool labelTruncated = false;
            for (int line = 0; line < 2; ++line) {
                if (labelLines[line].empty())
                    continue;
                ImVec2 labelSize = ImGui::CalcTextSize(labelLines[line].c_str());
                ImVec2 labelPos(tileMin.x + (cellSize - labelSize.x) * 0.5f,
                                iconMax.y + 8.0f + line * (fontSize + 2.0f));
                dl->AddText(labelPos, IM_COL32(205, 205, 205, 255), labelLines[line].c_str());
                if (labelLines[line].find("...") != std::string::npos)
                    labelTruncated = true;
            }
            if (hovered && labelTruncated)
                ImGui::SetTooltip("%s", displayName.c_str());

            ImGui::PopID();
            ImGui::NextColumn();
        }

        ImGui::Columns(1);

        // ── Delete key: batch delete selected assets ────────────────
        if (!m_SelectedAssets.empty() && ImGui::IsWindowFocused() &&
            ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            auto toDelete = m_SelectedAssets;
            m_SelectedAssets.clear();
            m_SelectedAssetPath.clear();
            for (auto& p : toDelete)
                m_AssetDatabase.DeleteAsset(p);
            ImGui::EndChild();
            ImGui::End();
            return;
        }

        // ── Box Selection (drag rectangle in empty space) ───────────
        {
            ImVec2 contentMin = ImGui::GetWindowPos();
            ImVec2 contentMax = { contentMin.x + ImGui::GetWindowWidth(),
                                   contentMin.y + ImGui::GetWindowHeight() };

            // Start box select on left-mouse-down in empty area
            if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                && !ImGui::IsAnyItemHovered() && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift) {
                m_BoxSelecting = true;
                m_BoxSelectStart = ImGui::GetMousePos();
                m_BoxSelectEnd = m_BoxSelectStart;
                if (!ImGui::GetIO().KeyCtrl)
                    m_SelectedAssets.clear();
            }

            if (m_BoxSelecting) {
                m_BoxSelectEnd = ImGui::GetMousePos();

                // Draw selection rectangle
                ImDrawList* fgDL = ImGui::GetWindowDrawList();
                ImVec2 rMin = { std::min(m_BoxSelectStart.x, m_BoxSelectEnd.x),
                                std::min(m_BoxSelectStart.y, m_BoxSelectEnd.y) };
                ImVec2 rMax = { std::max(m_BoxSelectStart.x, m_BoxSelectEnd.x),
                                std::max(m_BoxSelectStart.y, m_BoxSelectEnd.y) };
                fgDL->AddRectFilled(rMin, rMax, IM_COL32(100, 170, 255, 40));
                fgDL->AddRect(rMin, rMax, IM_COL32(100, 170, 255, 200));

                // Select items whose centers are inside the rectangle
                m_SelectedAssets.clear();
                for (auto& [path, center] : m_ContentItemPositions) {
                    if (center.x >= rMin.x && center.x <= rMax.x &&
                        center.y >= rMin.y && center.y <= rMax.y) {
                        m_SelectedAssets.insert(path);
                        m_SelectedAssetPath = path;
                    }
                }

                if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
                    m_BoxSelecting = false;
            }
        }

        ImGui::EndChild();

        ImGui::End();
    }

    void DrawFolderTree(const std::string& relDir) {
        auto contents = m_AssetDatabase.GetDirectoryContents(relDir);

        for (auto& relPath : contents) {
            auto* meta = m_AssetDatabase.GetMetaByPath(relPath);
            if (!meta || meta->Type != VE::AssetType::Folder) continue;

            std::string name = std::filesystem::path(relPath).filename().generic_string();

            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
            if (m_BrowserCurrentDir == relPath)
                flags |= ImGuiTreeNodeFlags_Selected;

            // Check if folder has subfolders
            auto sub = m_AssetDatabase.GetDirectoryContents(relPath);
            bool hasSubFolders = false;
            for (auto& s : sub) {
                auto* sm = m_AssetDatabase.GetMetaByPath(s);
                if (sm && sm->Type == VE::AssetType::Folder) { hasSubFolders = true; break; }
            }
            if (!hasSubFolders)
                flags |= ImGuiTreeNodeFlags_Leaf;

            bool opened = ImGui::TreeNodeEx(name.c_str(), flags);
            if (ImGui::IsItemClicked())
                m_BrowserCurrentDir = relPath;

            if (opened) {
                DrawFolderTree(relPath);
                ImGui::TreePop();
            }
        }
    }

    // ── Scene Info Panel ───────────────────────────────────────────────

    void DrawSceneInfoPanel() {
        if (!m_ShowSceneInfo) return;
        ImGui::Begin("Scene Info", &m_ShowSceneInfo);

        // ── Settings ──
        const char* apiNames[] = { "OpenGL", "Vulkan" };
        int currentAPI = (GetCurrentAPI() == VE::RendererAPI::API::OpenGL) ? 0 : 1;
        if (ImGui::Combo("Renderer", &currentAPI, apiNames, 2)) {
            RequestSwitchAPI(currentAPI == 0
                ? VE::RendererAPI::API::OpenGL : VE::RendererAPI::API::Vulkan);
        }

        const char* cameraModes[] = { "2D Orthographic", "3D Perspective" };
        int camMode = (m_Camera.GetMode() == VE::CameraMode::Perspective3D) ? 1 : 0;
        if (ImGui::Combo("Camera", &camMode, cameraModes, 2)) {
            m_Camera.SetMode(camMode == 0
                ? VE::CameraMode::Orthographic2D : VE::CameraMode::Perspective3D);
        }

        ImGui::Separator();
        ImGui::Text("Scene: %s", m_CurrentScenePath.empty() ? "(unsaved)" : m_CurrentScenePath.c_str());

        // ── Statistics (Unity-style) ──
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Statistics", ImGuiTreeNodeFlags_DefaultOpen)) {
            float fps = ImGui::GetIO().Framerate;
            float ms  = 1000.0f / fps;
            ImGui::Text("FPS: %.1f  (%.2f ms)", fps, ms);

            ImGui::Separator();
            const auto& stats = VE::RenderCommand::GetStats();
            auto spriteStats  = VE::SpriteBatchRenderer::GetStats();
            auto instanceStats = VE::InstancedRenderer::GetStats();

            uint32_t totalDrawCalls = stats.DrawCalls + spriteStats.DrawCalls + instanceStats.DrawCalls;
            uint32_t totalTris      = stats.Triangles + spriteStats.QuadCount * 2;
            uint32_t totalVerts     = stats.Vertices + spriteStats.QuadCount * 4;

            // Batches = draw calls saved by batching
            uint32_t batchedBy = instanceStats.InstanceCount > 0
                ? (instanceStats.InstanceCount - instanceStats.DrawCalls) : 0;

            int entityCount = static_cast<int>(m_Scene->GetRegistry().storage<entt::entity>().size());

            ImGui::Text("Batches:        %u", totalDrawCalls);
            ImGui::Text("  Draw Calls:   %u", totalDrawCalls);
            ImGui::Text("  Saved by batching: %u", batchedBy);
            ImGui::Text("Triangles:      %s", FormatNumber(totalTris).c_str());
            ImGui::Text("Vertices:       %s", FormatNumber(totalVerts).c_str());
            ImGui::Separator();
            ImGui::Text("Entities:       %d", entityCount);
            ImGui::Text("  Visible:      %u", stats.VisibleObjects);
            ImGui::Text("  Culled:       %u", stats.CulledObjects);
            ImGui::Text("  Occluded:     %u", stats.OccludedObjects);
            ImGui::Text("Sprites:        %u", spriteStats.QuadCount);
            ImGui::Text("Instances:      %u", instanceStats.InstanceCount);

        }

        // ── Camera ──
        if (ImGui::CollapsingHeader("Camera")) {
            if (m_Camera.GetMode() == VE::CameraMode::Perspective3D) {
                auto p = m_Camera.GetPosition3D();
                ImGui::Text("Position: (%.1f, %.1f, %.1f)", p.x, p.y, p.z);
                ImGui::Text("Distance: %.2f", m_Camera.GetDistance());
            } else {
                ImGui::Text("Position: (%.1f, %.1f)",
                    m_Camera.GetPosition().x, m_Camera.GetPosition().y);
                ImGui::Text("Zoom: %.2f", m_Camera.GetZoom());
            }
        }

        ImGui::End();
    }

    static std::string FormatNumber(uint32_t n) {
        if (n >= 1000000) {
            return std::to_string(n / 1000000) + "." + std::to_string((n % 1000000) / 100000) + "M";
        } else if (n >= 1000) {
            return std::to_string(n / 1000) + "." + std::to_string((n % 1000) / 100) + "k";
        }
        return std::to_string(n);
    }

    // ── Profiler Panel ───────────────────────────────────────────────────

    void DrawProfilerPanel() {
        if (!m_ShowProfiler) return;
        ImGui::Begin("Profiler", &m_ShowProfiler);

        const auto& stats = VE::Profiler::GetCurrentStats();

        // ── FPS / Frame Time header ──
        ImGui::Text("FPS: %.1f  (avg %.1f)", stats.FPS, VE::Profiler::GetAverageFPS());
        ImGui::Text("Frame: %.2f ms  (avg %.2f, min %.2f, max %.2f)",
            stats.FrameTimeMs,
            VE::Profiler::GetAverageFrameTime(),
            VE::Profiler::GetMinFrameTime(),
            VE::Profiler::GetMaxFrameTime());

        ImGui::Separator();

        // ── FPS Graph ──
        if (ImGui::CollapsingHeader("FPS Graph", ImGuiTreeNodeFlags_DefaultOpen)) {
            char overlay[32];
            snprintf(overlay, sizeof(overlay), "%.1f FPS", stats.FPS);
            ImGui::PlotLines("##fps", VE::Profiler::GetFPSHistory(),
                VE::Profiler::GetHistorySize(), VE::Profiler::GetHistoryOffset(),
                overlay, 0.0f, 240.0f, ImVec2(0, 80));
        }

        // ── Frame Time Graph ──
        if (ImGui::CollapsingHeader("Frame Time", ImGuiTreeNodeFlags_DefaultOpen)) {
            char overlay[32];
            snprintf(overlay, sizeof(overlay), "%.2f ms", stats.FrameTimeMs);
            ImGui::PlotLines("##frametime", VE::Profiler::GetFrameTimeHistory(),
                VE::Profiler::GetHistorySize(), VE::Profiler::GetHistoryOffset(),
                overlay, 0.0f, 33.3f, ImVec2(0, 80));
        }

        // ── Subsystem Breakdown ──
        if (ImGui::CollapsingHeader("Subsystem Breakdown", ImGuiTreeNodeFlags_DefaultOpen)) {
            float total = stats.FrameTimeMs > 0.001f ? stats.FrameTimeMs : 1.0f;

            struct SubsystemEntry { const char* Name; float Ms; ImU32 Color; };
            SubsystemEntry entries[] = {
                { "Render",  stats.RenderMs,  IM_COL32(66, 150, 250, 255) },
                { "Physics", stats.PhysicsMs, IM_COL32(250, 100, 80, 255) },
                { "Scripts", stats.ScriptsMs, IM_COL32(100, 220, 100, 255) },
                { "Audio",   stats.AudioMs,   IM_COL32(220, 180, 50, 255) },
                { "ImGui",   stats.ImGuiMs,   IM_COL32(180, 100, 220, 255) },
            };

            // Colored bar
            ImVec2 barPos = ImGui::GetCursorScreenPos();
            float barW = ImGui::GetContentRegionAvail().x;
            float barH = 20.0f;
            ImDrawList* dl = ImGui::GetWindowDrawList();

            float x = barPos.x;
            for (auto& e : entries) {
                float frac = e.Ms / total;
                float segW = frac * barW;
                if (segW > 0.5f) {
                    dl->AddRectFilled(ImVec2(x, barPos.y),
                        ImVec2(x + segW, barPos.y + barH), e.Color);
                    x += segW;
                }
            }
            // Fill remainder as "Other"
            if (x < barPos.x + barW) {
                dl->AddRectFilled(ImVec2(x, barPos.y),
                    ImVec2(barPos.x + barW, barPos.y + barH), IM_COL32(80, 80, 80, 255));
            }
            ImGui::Dummy(ImVec2(barW, barH + 4.0f));

            // Legend table
            if (ImGui::BeginTable("##subsystems", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
                ImGui::TableSetupColumn("Subsystem", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                ImGui::TableSetupColumn("Time (ms)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("%", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                for (auto& e : entries) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImVec4 col = ImGui::ColorConvertU32ToFloat4(e.Color);
                    ImGui::TextColored(col, "%s", e.Name);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%.3f", e.Ms);
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%.1f%%", (e.Ms / total) * 100.0f);
                }
                // "Other" row
                float accountedMs = stats.RenderMs + stats.PhysicsMs + stats.ScriptsMs + stats.AudioMs + stats.ImGuiMs;
                float otherMs = stats.FrameTimeMs - accountedMs;
                if (otherMs < 0.0f) otherMs = 0.0f;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Other");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.3f", otherMs);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.1f%%", (otherMs / total) * 100.0f);
                ImGui::EndTable();
            }
        }

        // ── Render Stats ──
        if (ImGui::CollapsingHeader("Render Stats", ImGuiTreeNodeFlags_DefaultOpen)) {
            const auto& rs = VE::RenderCommand::GetStats();
            auto spriteStats  = VE::SpriteBatchRenderer::GetStats();
            auto instanceStats = VE::InstancedRenderer::GetStats();

            uint32_t totalDrawCalls = rs.DrawCalls + spriteStats.DrawCalls + instanceStats.DrawCalls;
            uint32_t totalTris  = rs.Triangles + spriteStats.QuadCount * 2;
            uint32_t totalVerts = rs.Vertices  + spriteStats.QuadCount * 4;

            if (ImGui::BeginTable("##renderstats", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
                ImGui::TableSetupColumn("Stat", ImGuiTableColumnFlags_WidthFixed, 140.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                auto Row = [](const char* label, const std::string& val) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("%s", label);
                    ImGui::TableSetColumnIndex(1); ImGui::Text("%s", val.c_str());
                };

                Row("Draw Calls", std::to_string(totalDrawCalls));
                Row("Triangles", FormatNumber(totalTris));
                Row("Vertices", FormatNumber(totalVerts));
                Row("Entities", std::to_string(stats.EntityCount));
                Row("Visible Objects", std::to_string(rs.VisibleObjects));
                Row("Culled Objects", std::to_string(rs.CulledObjects));
                Row("SetPass Calls", std::to_string(rs.SetPassCalls));
                Row("Sprites", std::to_string(spriteStats.QuadCount));
                Row("Instances", std::to_string(instanceStats.InstanceCount));

                ImGui::EndTable();
            }
        }

        ImGui::End();
    }


    // ── Script Build Status Overlay ────────────────────────────────────

    // Generic build status overlay helper
    bool DrawBuildStatusOverlay(const char* windowID, int statusInt, float& fadeTimer,
                                float yOffset, const char* buildingText,
                                const char* successText, const char* failedText) {
        // statusInt: 0=Idle, 1=Building, 2=Success, 3=Failed
        static std::unordered_map<std::string, int> lastStatuses;
        auto& lastStatus = lastStatuses[windowID];
        if (statusInt != lastStatus) {
            lastStatus = statusInt;
            if (statusInt == 2 || statusInt == 3)
                fadeTimer = 3.0f;
        }

        bool show = false;
        if (statusInt == 1) {
            show = true;
        } else if ((statusInt == 2 || statusInt == 3) && fadeTimer > 0.0f) {
            fadeTimer -= m_DeltaTime;
            show = true;
        }
        if (!show) return false;

        ImGuiViewport* vp = ImGui::GetMainViewport();
        float width = 280.0f, height = 28.0f;
        ImVec2 pos(vp->WorkPos.x + vp->WorkSize.x - width - 10.0f,
                   vp->WorkPos.y + vp->WorkSize.y - height - 10.0f - yOffset);
        ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(width, height));

        float alpha = (fadeTimer < 1.0f && statusInt != 1) ? fadeTimer : 1.0f;
        ImGui::SetNextWindowBgAlpha(0.75f * alpha);

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs
            | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings
            | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoMove;

        if (ImGui::Begin(windowID, nullptr, flags)) {
            ImVec4 col;
            const char* text;
            switch (statusInt) {
                case 1: col = ImVec4(1,1,0.2f,alpha); text = buildingText; break;
                case 2: col = ImVec4(0.2f,1,0.2f,alpha); text = successText; break;
                case 3: col = ImVec4(1,0.2f,0.2f,alpha); text = failedText; break;
                default: col = ImVec4(1,1,1,alpha); text = ""; break;
            }
            ImGui::TextColored(col, "%s", text);
        }
        ImGui::End();
        return show;
    }

    void DrawScriptBuildStatus() {
        float yOff = 0.0f;

        // Script build status
        int scriptStatus = static_cast<int>(VE::ScriptEngine::GetBuildStatus());
        bool scriptShown = DrawBuildStatusOverlay("##ScriptBuild", scriptStatus,
            m_BuildStatusFadeTimer, yOff,
            ">> Building scripts...",
            "Scripts compiled + reloaded",
            "Script build failed");
        if (scriptShown) yOff += 34.0f;

        // Plugin build status
        static float pluginFadeTimer = 0.0f;
        int pluginStatus = static_cast<int>(VE::PluginEngine::GetBuildStatus());
        DrawBuildStatusOverlay("##PluginBuild", pluginStatus,
            pluginFadeTimer, yOff,
            ">> Building plugins...",
            "Plugins compiled + reloaded",
            "Plugin build failed");
    }

    // ── Input Settings Panel ────────────────────────────────────────────

    static const char* BindingSourceName(VE::BindingSource src) {
        switch (src) {
            case VE::BindingSource::Key:           return "Key";
            case VE::BindingSource::MouseButton:   return "MouseButton";
            case VE::BindingSource::GamepadButton: return "GamepadButton";
            case VE::BindingSource::GamepadAxis:   return "GamepadAxis";
            case VE::BindingSource::MouseAxisX:    return "MouseAxisX";
            case VE::BindingSource::MouseAxisY:    return "MouseAxisY";
            case VE::BindingSource::ScrollWheel:   return "ScrollWheel";
        }
        return "Unknown";
    }

    static const char* KeyCodeName(int code) {
        // Common keys
        if (code >= 65 && code <= 90) { static char buf[2]; buf[0] = (char)code; buf[1] = 0; return buf; }
        if (code >= 48 && code <= 57) { static char buf[2]; buf[0] = (char)code; buf[1] = 0; return buf; }
        switch (code) {
            case 32:  return "Space";   case 256: return "Escape"; case 257: return "Enter";
            case 258: return "Tab";     case 259: return "Backspace";
            case 262: return "Right";   case 263: return "Left";
            case 264: return "Down";    case 265: return "Up";
            case 340: return "LShift";  case 341: return "LCtrl";
            case 342: return "LAlt";    case 344: return "RShift";
            case 345: return "RCtrl";   case 346: return "RAlt";
        }
        static char buf[16]; snprintf(buf, sizeof(buf), "%d", code); return buf;
    }

    static const char* GamepadButtonName(int code) {
        const char* names[] = { "A", "B", "X", "Y", "LB", "RB", "Back", "Start",
                                "Guide", "LStick", "RStick", "Up", "Right", "Down", "Left" };
        return (code >= 0 && code < 15) ? names[code] : "?";
    }

    static const char* GamepadAxisName(int code) {
        const char* names[] = { "LeftX", "LeftY", "RightX", "RightY", "LTrigger", "RTrigger" };
        return (code >= 0 && code < 6) ? names[code] : "?";
    }

    void DrawInputSettingsPanel() {
        if (!m_ShowInputSettings) return;
        ImGui::Begin("Input Settings", &m_ShowInputSettings);

        // ── Gamepad status ──
        if (ImGui::CollapsingHeader("Gamepad Status", ImGuiTreeNodeFlags_DefaultOpen)) {
            for (int i = 0; i < VE::Input::MAX_GAMEPADS; ++i) {
                if (VE::Input::IsGamepadConnected(i)) {
                    ImGui::TextColored({0.3f,1.0f,0.3f,1.0f}, "Gamepad %d: %s",
                        i, VE::Input::GetGamepadName(i).c_str());

                    // Show live axis values
                    ImGui::Indent();
                    for (int a = 0; a < 6; ++a) {
                        float val = VE::Input::GetGamepadAxis(static_cast<VE::GamepadAxis>(a), i);
                        ImGui::Text("  %s: %.2f", GamepadAxisName(a), val);
                        ImGui::SameLine(200);
                        ImGui::ProgressBar((val + 1.0f) * 0.5f, ImVec2(100, 0));
                    }
                    // Show active buttons
                    std::string activeButtons;
                    for (int b = 0; b < 15; ++b) {
                        if (VE::Input::IsGamepadButtonDown(static_cast<VE::GamepadButton>(b), i)) {
                            if (!activeButtons.empty()) activeButtons += " ";
                            activeButtons += GamepadButtonName(b);
                        }
                    }
                    if (!activeButtons.empty())
                        ImGui::TextColored({1,1,0,1}, "  Pressed: %s", activeButtons.c_str());
                    ImGui::Unindent();
                } else {
                    ImGui::TextDisabled("Gamepad %d: Not connected", i);
                }
            }
        }

        // ── Deadzone ──
        float dz = VE::Input::GetDeadzone();
        if (ImGui::SliderFloat("Stick Deadzone", &dz, 0.0f, 0.5f))
            VE::Input::SetDeadzone(dz);

        ImGui::Separator();

        // ── Action Maps ──
        if (ImGui::CollapsingHeader("Action Maps", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto& maps = VE::InputActions::GetAllMapsMutable();
            for (size_t mi = 0; mi < maps.size(); ++mi) {
                auto& map = maps[mi];
                bool enabled = map.IsEnabled();
                std::string mapHeader = map.GetName() + (enabled ? "" : " (disabled)");
                bool mapOpen = ImGui::TreeNode(("##map" + std::to_string(mi)).c_str(), "%s", mapHeader.c_str());
                ImGui::SameLine(ImGui::GetWindowWidth() - 80);
                if (ImGui::Checkbox(("##en" + std::to_string(mi)).c_str(), &enabled))
                    map.SetEnabled(enabled);

                if (mapOpen) {
                    for (auto& action : map.GetActions()) {
                        float val = action.GetValue();
                        bool down = action.IsDown();
                        ImVec4 col = down ? ImVec4(0.3f,1,0.3f,1) : ImVec4(0.7f,0.7f,0.7f,1);
                        ImGui::TextColored(col, "  %s = %.2f", action.GetName().c_str(), val);

                        // Show bindings
                        ImGui::Indent();
                        for (const auto& b : action.GetBindings()) {
                            const char* srcName = BindingSourceName(b.Source);
                            std::string codeName;
                            switch (b.Source) {
                                case VE::BindingSource::Key:           codeName = KeyCodeName(b.Code); break;
                                case VE::BindingSource::MouseButton:   codeName = std::to_string(b.Code); break;
                                case VE::BindingSource::GamepadButton: codeName = GamepadButtonName(b.Code); break;
                                case VE::BindingSource::GamepadAxis:   codeName = GamepadAxisName(b.Code); break;
                                default: codeName = "-"; break;
                            }
                            ImGui::TextDisabled("    [%s] %s (x%.1f)", srcName, codeName.c_str(), b.Scale);
                        }
                        ImGui::Unindent();
                    }
                    ImGui::TreePop();
                }
            }
        }

        // ── Save / Load ──
        ImGui::Separator();
        if (ImGui::Button("Save Input Map")) {
            auto* map = VE::InputActions::GetMap("Player");
            if (map) map->SaveToFile("ProjectSettings/InputActions.yaml");
        }
        ImGui::SameLine();
        if (ImGui::Button("Load Input Map")) {
            auto* map = VE::InputActions::GetMap("Player");
            if (map) map->LoadFromFile("ProjectSettings/InputActions.yaml");
        }

        ImGui::End();
    }

private:
    std::shared_ptr<VE::Scene> m_Scene;
    VE::Entity m_SelectedEntity;                         // primary selection (Inspector/gizmo target)
    std::set<entt::entity> m_SelectedEntities;           // all selected entities
    entt::entity m_LastClickedEntity = entt::null;       // for Shift+Click range select in hierarchy

    // Viewport box selection state
    bool m_EntityBoxSelecting = false;
    ImVec2 m_EntityBoxSelectStart = { 0, 0 };
    ImVec2 m_EntityBoxSelectEnd = { 0, 0 };

    VE::EditorCamera m_Camera;
    glm::mat4 m_FrameVP = glm::mat4(1.0f);
    glm::mat4 m_PrevFrameVP = glm::mat4(1.0f);
    std::string m_CurrentScenePath;
    std::shared_ptr<VE::Framebuffer> m_Framebuffer;
    uint32_t m_CurrentMSAASamples = 1; // track to detect changes
    bool m_ViewportHovered = false;
    bool m_ViewportFocused = false;
    bool m_GizmosEnabled   = true;
    SceneGizmoTool m_GizmoTool = SceneGizmoTool::Translate;
    bool m_OutlineEnabled  = true;
    float m_SceneVpX = 0, m_SceneVpY = 0, m_SceneVpW = 1, m_SceneVpH = 1;

    // Gizmo drag state
    VE::GizmoAxis m_DraggingAxis = VE::GizmoAxis::None;
    glm::vec3 m_DragStartPos = { 0.0f, 0.0f, 0.0f };       // local position at drag start
    glm::vec3 m_DragWorldStartPos = { 0.0f, 0.0f, 0.0f };   // world position at drag start
    glm::mat3 m_DragWorldRot = glm::mat3(1.0f);              // world rotation at drag start
    glm::mat4 m_DragParentInverse = glm::mat4(1.0f);         // inverse of parent's world transform
    float m_DragOriginVal = 0.0f;
    std::unordered_map<entt::entity, glm::vec3> m_BatchDragStartPositions; // for batch move

    std::unordered_map<uint32_t, int> m_EntityMeshIndex;

    bool m_ShowHierarchy = true;
    bool m_ShowInspector = true;
    bool m_ShowSceneInfo = true;
    bool m_ShowPipelineSettings = false;
    bool m_ShowRenderDebugger = true;
    bool m_AutoExportRenderDiagnostics = true;
    bool m_RenderDiagnosticsOnce = false;
    bool m_HPWaterMotionDiagnostics = false;
    bool m_HPWaterFluidFilterDiagnostics = false;
    bool m_HPWaterSSRDiagnostics = false;
    bool m_RenderDiagnosticsRequireObjectMotion = false;
    bool m_RenderDiagnosticsRequireFluidFiltering = false;
    bool m_RenderDiagnosticsRequireSSR = false;
    uint64_t m_RenderDiagnosticsOnceMinFrame = 24;
    uint64_t m_RenderDiagnosticsOnceMaxFrame = 180;
    uint64_t m_LastAutoRenderDiagnosticFrame = 0;
    float m_HPWaterMotionDiagnosticTime = 0.0f;
    float m_HPWaterFluidFilterDiagnosticTime = 0.0f;
    bool m_ShowContentBrowser = true;
    bool m_ShowScripting = false;
    bool m_ShowProjectSettings = false;
    bool m_ShowInputSettings = false;
    bool m_ShowBuildPanel = false;
    bool m_ShowMaterialEditor = false;

    bool m_ShowProfiler = false;
    bool m_ShowColliders = true;

    // Add Component search
    char m_AddComponentSearch[128] = {};
    bool m_AddComponentFocusSearch = false;

    bool m_ShowSceneManager = false;


    // Scene Manager
    VE::SceneManager m_SceneManager;

    // Build settings
    VE::BuildSettings m_BuildSettings;
    std::string m_BuildLog;
    bool m_BuildInProgress = false;

    // LOD test scene mesh storage (keep alive)
    std::vector<std::shared_ptr<VE::MeshAsset>> m_LODMeshes;

    // Project settings: custom tags & layers
    std::vector<std::string> m_CustomTags = {
        "Untagged", "Respawn", "Finish", "EditorOnly",
        "MainCamera", "Player", "GameController"
    };
    std::array<std::string, 32> m_LayerNames = {
        "Default", "TransparentFX", "Ignore Raycast", "",
        "Water", "UI", "", "", "",
        "", "", "", "", "", "", "",
        "", "", "", "", "", "", "", "",
        "", "", "", "", "", "", "", ""
    };

    // Play mode
    bool m_PlayMode = false;
    bool m_Paused = false;
    bool m_StepOneFrame = false;
    std::string m_SceneSnapshot;
    std::vector<VE::SceneManager::SceneSnapshot> m_SceneManagerSnapshots;

    // Scripting
    std::string m_ScriptDLLPath;
    float m_BuildStatusFadeTimer = 0.0f;

    // Asset management
    VE::AssetDatabase m_AssetDatabase;
    VE::ThumbnailCache m_ThumbnailCache;
    std::string m_BrowserCurrentDir; // relative to Assets/
    std::string m_SelectedAssetPath; // primary selected asset in Content Browser
    std::set<std::string> m_SelectedAssets; // multi-select set
    char m_ContentSearch[128] = {};
    float m_ContentIconSize = 104.0f;
    // Box selection state
    bool m_BoxSelecting = false;
    ImVec2 m_BoxSelectStart = { 0, 0 };
    ImVec2 m_BoxSelectEnd = { 0, 0 };
    std::vector<std::pair<std::string, ImVec2>> m_ContentItemPositions; // for box select hit test
    std::shared_ptr<VE::Material> m_InspectedMaterial;
    std::string m_InspectedMaterialPath;

    // Mesh asset inspector
    VE::FBXImportSettings m_InspectedMeshSettings;
    std::string m_InspectedMeshPath;
    bool m_MeshSettingsDirty = false;

    // Post-processing
    VE::PostProcessing m_PostProcessing;
    uint32_t m_PostProcessedTexture = 0;
    VE::SSAO m_SSAO;
    VE::SSAO m_GameSSAO;
    VE::SSR  m_SSR;
    VE::SSR  m_GameSSR;

    // Game viewport
    bool m_ShowGameView = false;
    std::shared_ptr<VE::Framebuffer> m_GameFramebuffer;
    VE::PostProcessing m_GamePostProcessing;
    uint32_t m_GamePostProcessedTexture = 0;

    // Undo system
    VE::CommandHistory m_CommandHistory;

    // Component clipboard
    std::string m_ClipboardComponentType;
    std::any    m_ClipboardComponentData;

    // Entity clipboard (copy/paste)
    std::string m_EntityClipboard;

    // Auto-save and crash recovery
    float m_AutoSaveTimer = 0.0f;
    float m_AutoSaveInterval = 120.0f; // seconds (2 minutes default)
    bool  m_AutoSaveEnabled = true;
    bool  m_SceneDirty = false;
    bool  m_ShowRecoveryPopup = false;
    bool  m_RecoveryCheckDone = false;
};

int main(int argc, char** argv) {
    bool renderDiagnosticsOnce = false;
    bool hpwaterMotionDiagnostics = false;
    bool hpwaterFluidFilterDiagnostics = false;
    bool hpwaterSSRDiagnostics = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--render-diagnostics-once") {
            renderDiagnosticsOnce = true;
        } else if (arg == "--hpwater-motion-diagnostics") {
            renderDiagnosticsOnce = true;
            hpwaterMotionDiagnostics = true;
        } else if (arg == "--hpwater-fluid-filter-diagnostics") {
            renderDiagnosticsOnce = true;
            hpwaterFluidFilterDiagnostics = true;
        } else if (arg == "--hpwater-ssr-diagnostics") {
            renderDiagnosticsOnce = true;
            hpwaterSSRDiagnostics = true;
        }
    }

    Sandbox app(renderDiagnosticsOnce, hpwaterMotionDiagnostics, hpwaterFluidFilterDiagnostics, hpwaterSSRDiagnostics);
    app.Run();
    return 0;
}
