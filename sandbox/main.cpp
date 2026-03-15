#include <VibeEngine/VibeEngine.h>
#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <yaml-cpp/yaml.h>
#include <limits>
#include <filesystem>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <any>
#include <set>

static const char* s_SceneFilter = "VibeEngine Scene (*.vscene)\0*.vscene\0All Files\0*.*\0";

class Sandbox : public VE::Application {
public:
    Sandbox()
        : VE::Application(VE::RendererAPI::API::OpenGL)
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
        VE::InstancedRenderer::Init();
        VE::UIRenderer::Init();
        m_Scene = std::make_shared<VE::Scene>();
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
            m_SelectedEntity = {};
            uint64_t uuid = m_CommandHistory.GetSelectedEntityUUID();
            if (uuid != 0) {
                auto view = m_Scene->GetAllEntitiesWith<VE::IDComponent>();
                for (auto e : view) {
                    auto& id = view.get<VE::IDComponent>(e);
                    if (static_cast<uint64_t>(id.ID) == uuid) {
                        m_SelectedEntity = VE::Entity(e, &*m_Scene);
                        break;
                    }
                }
            }
        });

        // Set up default input action map
        SetupDefaultInputActions();

        // Restore last session (scene + camera)
        LoadEditorSettings();
    }

    ~Sandbox() override {
        SaveEditorSettings();
        VE::UIRenderer::Shutdown();
        VE::InstancedRenderer::Shutdown();
        VE::SpriteBatchRenderer::Shutdown();
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

        if (shouldUpdate)
            m_Scene->OnUpdate(m_DeltaTime);

        if (m_StepOneFrame) {
            m_StepOneFrame = false;
            m_Paused = true;
        }

        m_AssetDatabase.Update(m_DeltaTime);
        if (m_PlayMode) {
            VE::ScriptEngine::CheckForReload();

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

        auto shader = VE::MeshLibrary::GetDefaultShader();
        if (!shader) return;

        glm::mat4 model = m_Scene->GetWorldTransform(m_SelectedEntity.GetHandle());
        glm::mat4 mvp = m_FrameVP * model;

        // Pass 1: draw the mesh into stencil buffer only (no color/depth writes)
        glEnable(GL_STENCIL_TEST);
        glDepthFunc(GL_LEQUAL);
        glStencilFunc(GL_ALWAYS, 1, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        glStencilMask(0xFF);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glDepthMask(GL_FALSE);

        shader->Bind();
        shader->SetMat4("u_MVP", mvp);
        shader->SetInt("u_UseTexture", 0);
        shader->SetVec4("u_EntityColor", glm::vec4(1.0f));
        vao->Bind();
        glDrawElements(GL_TRIANGLES,
            static_cast<GLsizei>(vao->GetIndexBuffer()->GetCount()),
            GL_UNSIGNED_INT, nullptr);

        // Pass 2: draw scaled-up mesh in outline color, only where stencil != 1
        glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
        glStencilMask(0x00);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDisable(GL_DEPTH_TEST);

        VE::AABB localBox = VE::AABB{ glm::vec3(-0.5f), glm::vec3(0.5f) };
        if (m_SelectedEntity.HasComponent<VE::MeshRendererComponent>())
            localBox = GetEntityLocalAABB(m_SelectedEntity.GetComponent<VE::MeshRendererComponent>());
        glm::vec3 center = localBox.Center();

        float outlineScale = 1.05f;
        glm::mat4 scaledModel = model
            * glm::translate(glm::mat4(1.0f), center)
            * glm::scale(glm::mat4(1.0f), glm::vec3(outlineScale))
            * glm::translate(glm::mat4(1.0f), -center);
        glm::mat4 scaledMVP = m_FrameVP * scaledModel;

        shader->SetMat4("u_MVP", scaledMVP);
        shader->SetVec4("u_EntityColor", glm::vec4(1.0f, 0.5f, 0.0f, 1.0f));
        glDrawElements(GL_TRIANGLES,
            static_cast<GLsizei>(vao->GetIndexBuffer()->GetCount()),
            GL_UNSIGNED_INT, nullptr);

        // Restore GL state
        glStencilMask(0xFF);
        glStencilFunc(GL_ALWAYS, 0, 0xFF);
        glDisable(GL_STENCIL_TEST);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
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
        s.Tonemap = { ps.TonemapEnabled, static_cast<VE::TonemapMode>(ps.TonemapMode) };
        // Fog
        s.Fog = { ps.FogEnabled, static_cast<VE::FogMode>(ps.FogMode), ps.FogColor,
                  ps.FogDensity, ps.FogStart, ps.FogEnd, ps.FogHeightFalloff, ps.FogMaxOpacity };
        // Volumetric fog
        s.VolumetricFog = { ps.VolFogEnabled, ps.VolFogDensity, ps.VolFogScattering,
                            ps.VolFogLightIntensity, ps.VolFogColor, ps.VolFogSteps,
                            ps.VolFogMaxDistance, ps.VolFogHeightFalloff, ps.VolFogBaseHeight };
        s.NearClip = m_Camera.GetNearClip();
        s.FarClip  = m_Camera.GetFarClip();
        s.InvProjection = glm::inverse(m_Camera.GetProjectionMatrix());
        s.InvView       = glm::inverse(m_Camera.GetViewMatrix());
        // Find directional light for volumetric fog god rays
        {
            auto lv = m_Scene->GetAllEntitiesWith<VE::DirectionalLightComponent>();
            for (auto e : lv) {
                auto& dl = lv.get<VE::DirectionalLightComponent>(e);
                s.LightDir = glm::normalize(glm::vec3(dl.Direction[0], dl.Direction[1], dl.Direction[2]));
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
        VE::RGHandle shadowMap, sceneColor;

        // Pass 0: Shadow depth (3D perspective only)
        if (perspective3D) {
            rg.AddPass("ShadowDepth", [&](VE::RGBuilder& b) {
                shadowMap = b.Import("ShadowMap",
                    m_Scene->GetShadowMap() ? m_Scene->GetShadowMap()->GetDepthTextureID() : 0,
                    VE::ShadowMap::MAP_SIZE, VE::ShadowMap::MAP_SIZE);
                b.Write(shadowMap);
                b.SideEffect();
            }, [&](const VE::RGResources&) {
                m_Scene->ComputeShadows(m_Camera.GetViewMatrix(),
                                        m_Camera.GetProjectionMatrix(),
                                        m_Camera.GetNearClip(),
                                        m_Camera.GetFarClip());
            });
        }

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
            m_Scene->OnRenderSky(m_Camera.GetSkyViewProjection());
        });

        // Pass 2: Opaque geometry
        rg.AddPass("Opaque", [&](VE::RGBuilder& b) {
            if (shadowMap.IsValid()) b.Read(shadowMap);
            b.Write(sceneColor);
            b.SideEffect();
        }, [&](const VE::RGResources&) {
            m_Scene->OnRender(m_FrameVP, camPos);
            m_Scene->OnRenderTerrain(m_FrameVP, camPos);
        });

        // Pass 3: Sprites
        rg.AddPass("Sprites", [&](VE::RGBuilder& b) {
            b.Write(sceneColor);
            b.SideEffect();
        }, [&](const VE::RGResources&) {
            m_Scene->OnRenderSprites(m_FrameVP);
        });

        // Pass 4: Particles
        rg.AddPass("Particles", [&](VE::RGBuilder& b) {
            b.Write(sceneColor);
            b.SideEffect();
        }, [&](const VE::RGResources&) {
            m_Scene->OnRenderParticles(m_FrameVP, camPos);
        });

        // Pass 5: Outline (conditional on selection)
        if (m_OutlineEnabled && m_SelectedEntity) {
            rg.AddPass("Outline", [&](VE::RGBuilder& b) {
                b.Write(sceneColor);
                b.SideEffect();
            }, [&](const VE::RGResources&) {
                RenderSelectedOutline();
            });
        }

        // Pass 6: PostProcess
        rg.AddPass("PostProcess", [&](VE::RGBuilder& b) {
            b.Read(sceneColor);
            b.SideEffect();
        }, [&](const VE::RGResources&) {
            if (m_Framebuffer) {
                m_Framebuffer->Unbind();
                if (m_Framebuffer->IsMultisampled())
                    m_Framebuffer->Resolve();

                auto ppSettings = BuildPostProcessSettings();
                uint32_t depthTex = static_cast<uint32_t>(m_Framebuffer->GetDepthAttachmentID());

                // Pass depth for fog
                ppSettings.DepthTexture = depthTex;

                // Compute SSAO if enabled
                auto& ps = m_Scene->GetPipelineSettings();
                if (ps.SSAOEnabled) {
                    VE::SSAOSettings ssaoSettings{ true, ps.SSAORadius, ps.SSAOBias,
                                                    ps.SSAOIntensity, ps.SSAOKernelSize };
                    ppSettings.SSAOTexture = m_SSAO.Compute(
                        depthTex, m_Framebuffer->GetWidth(), m_Framebuffer->GetHeight(),
                        m_Camera.GetProjectionMatrix(), m_Camera.GetViewMatrix(), ssaoSettings);
                }

                uint32_t sceneTex = static_cast<uint32_t>(m_Framebuffer->GetColorAttachmentID());
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
                    glm::mat4 skyView = glm::mat4(glm::mat3(gameView));
                    m_Scene->OnRenderSky(gameProj * skyView);
                });

                // Game Pass 1: Opaque
                rg.AddPass("GameOpaque", [&](VE::RGBuilder& b) {
                    b.Write(gameSceneColor);
                    b.SideEffect();
                }, [&](const VE::RGResources&) {
                    m_Scene->OnRender(gameVP, gameCamPos);
                    m_Scene->OnRenderTerrain(gameVP, gameCamPos);
                });

                // Game Pass 2: Sprites
                rg.AddPass("GameSprites", [&](VE::RGBuilder& b) {
                    b.Write(gameSceneColor);
                    b.SideEffect();
                }, [&](const VE::RGResources&) {
                    m_Scene->OnRenderSprites(gameVP);
                });

                // Game Pass 3: Particles
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

                    auto& gps = m_Scene->GetPipelineSettings();
                    if (gps.SSAOEnabled) {
                        VE::SSAOSettings ssaoS{ true, gps.SSAORadius, gps.SSAOBias,
                                                 gps.SSAOIntensity, gps.SSAOKernelSize };
                        ppSettings.SSAOTexture = m_GameSSAO.Compute(
                            gDepthTex, m_GameFramebuffer->GetWidth(), m_GameFramebuffer->GetHeight(),
                            gameProj, gameView, ssaoS);
                    }

                    uint32_t gameTex = static_cast<uint32_t>(m_GameFramebuffer->GetColorAttachmentID());
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
            if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D, false) && m_SelectedEntity.GetHandle() != entt::null)
                DuplicateSelectedEntity();
        }

        // ── Gizmo drag continuation (runs even over ImGui panels) ────
        if (m_DraggingAxis != VE::GizmoAxis::None) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                auto& tc = m_SelectedEntity.GetComponent<VE::TransformComponent>();
                glm::vec3 pos(tc.Position[0], tc.Position[1], tc.Position[2]);
                glm::mat3 rot = GetEntityRotation(tc);
                float val = VE::GizmoRenderer::ProjectMouseOntoAxis(
                    m_DraggingAxis, pos, io.MousePos.x, io.MousePos.y, rot);
                int comp = (m_DraggingAxis == VE::GizmoAxis::X) ? 0
                         : (m_DraggingAxis == VE::GizmoAxis::Y) ? 1 : 2;
                tc.Position[comp] = m_DragStartPos[comp] + (val - m_DragOriginVal);
            } else {
                if (!m_PlayMode) m_CommandHistory.EndPropertyEdit();
                m_DraggingAxis = VE::GizmoAxis::None;
            }
        }

        // ── Camera + scene interaction (only when viewport is hovered) ───
        if (m_ViewportHovered && m_DraggingAxis == VE::GizmoAxis::None) {
            if (io.MouseWheel != 0.0f)
                m_Camera.OnMouseScroll(io.MouseWheel);
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
                m_Camera.OnMouseDrag(io.MouseDelta.x, io.MouseDelta.y);
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Right))
                m_Camera.OnMouseRotate(io.MouseDelta.x, io.MouseDelta.y);

            // Focus camera on selected entity (F key) — zoom in and align to object's +Z
            if (ImGui::IsKeyPressed(ImGuiKey_F) && m_SelectedEntity && m_SelectedEntity.HasComponent<VE::TransformComponent>()) {
                auto& tc = m_SelectedEntity.GetComponent<VE::TransformComponent>();
                glm::vec3 pos(tc.Position[0], tc.Position[1], tc.Position[2]);
                float objectSize = std::max({tc.Scale[0], tc.Scale[1], tc.Scale[2]});
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
                    auto& tc = m_SelectedEntity.GetComponent<VE::TransformComponent>();
                    glm::vec3 pos(tc.Position[0], tc.Position[1], tc.Position[2]);
                    glm::mat3 rot = GetEntityRotation(tc);
                    axis = VE::GizmoRenderer::HitTestTranslationGizmo(
                        pos, io.MousePos.x, io.MousePos.y, 12.0f, rot);
                }

                if (axis != VE::GizmoAxis::None) {
                    if (!m_PlayMode) m_CommandHistory.BeginPropertyEdit("Move Entity");
                    m_DraggingAxis = axis;
                    auto& tc = m_SelectedEntity.GetComponent<VE::TransformComponent>();
                    glm::vec3 pos(tc.Position[0], tc.Position[1], tc.Position[2]);
                    glm::mat3 rot = GetEntityRotation(tc);
                    m_DragStartPos = { tc.Position[0], tc.Position[1], tc.Position[2] };
                    m_DragOriginVal = VE::GizmoRenderer::ProjectMouseOntoAxis(
                        axis, pos, io.MousePos.x, io.MousePos.y, rot);
                } else {
                    m_SelectedEntity = HitTestEntities(io.MousePos.x, io.MousePos.y);
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
        DrawScriptingPanel();
        DrawContentBrowserPanel();
        DrawProjectSettingsPanel();
        DrawGameViewPanel();
        DrawInputSettingsPanel();
        DrawBuildPanel();
        DrawProfilerPanel();
        DrawFPSOverlay();

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
    // ── Scene picking ─────────────────────────────────────────────────

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
            // Create a default material file
            auto mat = VE::Material::Create(name, VE::MeshLibrary::GetLitShader());
            mat->SetLit(true);
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

    void NewScene() {
        m_Scene = std::make_shared<VE::Scene>();
        m_SelectedEntity = {};
        m_CurrentScenePath.clear();
        m_CommandHistory.Clear();
        VE_INFO("New scene created");
    }

    void SaveScene() {
        if (m_CurrentScenePath.empty()) { SaveSceneAs(); return; }
        VE::SceneSerializer serializer(m_Scene);
        serializer.Serialize(m_CurrentScenePath);
    }

    void SaveSceneAs() {
        std::string path = VE::FileDialog::SaveFile(s_SceneFilter, GetWindow().GetNativeWindow());
        if (!path.empty()) {
            m_CurrentScenePath = path;
            VE::SceneSerializer serializer(m_Scene);
            serializer.Serialize(m_CurrentScenePath);
        }
    }

    void LoadScene() {
        std::string path = VE::FileDialog::OpenFile(s_SceneFilter, GetWindow().GetNativeWindow());
        if (!path.empty()) {
            m_Scene = std::make_shared<VE::Scene>();
            m_SelectedEntity = {};
            VE::SceneSerializer serializer(m_Scene);
            if (serializer.Deserialize(path))
                m_CurrentScenePath = path;
            m_CommandHistory.Clear();
        }
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
        out << YAML::Key << "ShowProfiler" << YAML::Value << m_ShowProfiler;
        out << YAML::Key << "ShowFPSOverlay" << YAML::Value << m_ShowFPSOverlay;

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

            // Restore last scene
            if (root["LastScene"]) {
                std::string scenePath = root["LastScene"].as<std::string>("");
                if (!scenePath.empty() && std::filesystem::exists(scenePath)) {
                    m_Scene = std::make_shared<VE::Scene>();
                    VE::SceneSerializer serializer(m_Scene);
                    if (serializer.Deserialize(scenePath))
                        m_CurrentScenePath = scenePath;
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
            if (root["ShowProfiler"])
                m_ShowProfiler = root["ShowProfiler"].as<bool>(false);
            if (root["ShowFPSOverlay"])
                m_ShowFPSOverlay = root["ShowFPSOverlay"].as<bool>(false);

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

    // ── Duplicate Entity ─────────────────────────────────────────────

    void DuplicateSelectedEntity() {
        if (m_SelectedEntity.GetHandle() == entt::null) return;

        m_CommandHistory.Execute("Duplicate Entity", [this]() {
            auto& reg = m_Scene->GetRegistry();
            auto srcEntity = m_SelectedEntity.GetHandle();

            auto& srcTag = reg.get<VE::TagComponent>(srcEntity);
            auto newEntity = m_Scene->CreateEntity(srcTag.Tag + " (Copy)");
            auto dst = newEntity.GetHandle();

            // Copy TransformComponent
            if (reg.any_of<VE::TransformComponent>(srcEntity))
                reg.get_or_emplace<VE::TransformComponent>(dst) = reg.get<VE::TransformComponent>(srcEntity);

            // Copy MeshRendererComponent
            if (reg.any_of<VE::MeshRendererComponent>(srcEntity))
                reg.get_or_emplace<VE::MeshRendererComponent>(dst) = reg.get<VE::MeshRendererComponent>(srcEntity);

            // Copy DirectionalLightComponent
            if (reg.any_of<VE::DirectionalLightComponent>(srcEntity))
                reg.get_or_emplace<VE::DirectionalLightComponent>(dst) = reg.get<VE::DirectionalLightComponent>(srcEntity);

            // Copy PointLightComponent
            if (reg.any_of<VE::PointLightComponent>(srcEntity))
                reg.get_or_emplace<VE::PointLightComponent>(dst) = reg.get<VE::PointLightComponent>(srcEntity);

            // Copy SpotLightComponent
            if (reg.any_of<VE::SpotLightComponent>(srcEntity))
                reg.get_or_emplace<VE::SpotLightComponent>(dst) = reg.get<VE::SpotLightComponent>(srcEntity);

            // Copy RigidbodyComponent (reset runtime body ID)
            if (reg.any_of<VE::RigidbodyComponent>(srcEntity)) {
                auto rb = reg.get<VE::RigidbodyComponent>(srcEntity);
                rb._JoltBodyID = 0xFFFFFFFF;
                reg.get_or_emplace<VE::RigidbodyComponent>(dst) = rb;
            }

            // Copy colliders
            if (reg.any_of<VE::BoxColliderComponent>(srcEntity))
                reg.get_or_emplace<VE::BoxColliderComponent>(dst) = reg.get<VE::BoxColliderComponent>(srcEntity);
            if (reg.any_of<VE::SphereColliderComponent>(srcEntity))
                reg.get_or_emplace<VE::SphereColliderComponent>(dst) = reg.get<VE::SphereColliderComponent>(srcEntity);
            if (reg.any_of<VE::CapsuleColliderComponent>(srcEntity))
                reg.get_or_emplace<VE::CapsuleColliderComponent>(dst) = reg.get<VE::CapsuleColliderComponent>(srcEntity);
            if (reg.any_of<VE::MeshColliderComponent>(srcEntity))
                reg.get_or_emplace<VE::MeshColliderComponent>(dst) = reg.get<VE::MeshColliderComponent>(srcEntity);

            // Copy CameraComponent
            if (reg.any_of<VE::CameraComponent>(srcEntity))
                reg.get_or_emplace<VE::CameraComponent>(dst) = reg.get<VE::CameraComponent>(srcEntity);

            // Copy ScriptComponent (reset runtime instance)
            if (reg.any_of<VE::ScriptComponent>(srcEntity)) {
                auto sc = reg.get<VE::ScriptComponent>(srcEntity);
                sc._Instance = nullptr;
                reg.get_or_emplace<VE::ScriptComponent>(dst) = sc;
            }

            m_SelectedEntity = newEntity;
        });
    }

    // ── Main Menu Bar ──────────────────────────────────────────────────

    void DrawMainMenuBar() {
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("New Scene", "Ctrl+N")) NewScene();
                if (ImGui::MenuItem("Open Scene", "Ctrl+O")) LoadScene();
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
                if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, m_SelectedEntity.GetHandle() != entt::null))
                    DuplicateSelectedEntity();
                if (ImGui::MenuItem("Delete", "Del", false, m_SelectedEntity.GetHandle() != entt::null)) {
                    m_CommandHistory.Execute("Delete Entity", [this]() {
                        m_Scene->DestroyEntity(m_SelectedEntity);
                        m_SelectedEntity = {};
                    });
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Settings")) {
                ImGui::MenuItem("Project Settings", nullptr, &m_ShowProjectSettings);
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
                    // Select first entity if none selected (no multi-select yet)
                    auto& reg = m_Scene->GetRegistry();
                    auto view = reg.view<VE::TagComponent>();
                    if (!view.empty())
                        m_SelectedEntity = VE::Entity(*view.begin(), &*m_Scene);
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
                        e.AddComponent<VE::DirectionalLightComponent>();
                    });
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                ImGui::MenuItem("Hierarchy", nullptr, &m_ShowHierarchy);
                ImGui::MenuItem("Inspector", nullptr, &m_ShowInspector);
                ImGui::MenuItem("Scene Info", nullptr, &m_ShowSceneInfo);
                ImGui::Separator();
                ImGui::MenuItem("Render Pipeline", nullptr, &m_ShowPipelineSettings);
                ImGui::MenuItem("Scripting", nullptr, &m_ShowScripting);
                ImGui::MenuItem("Content Browser", nullptr, &m_ShowContentBrowser);
                ImGui::MenuItem("Game", nullptr, &m_ShowGameView);
                ImGui::MenuItem("Input Settings", nullptr, &m_ShowInputSettings);
                ImGui::Separator();
                ImGui::MenuItem("Profiler", nullptr, &m_ShowProfiler);
                ImGui::MenuItem("FPS Overlay", nullptr, &m_ShowFPSOverlay);
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

            // Right to left: Gizmos, Outline
            DrawToggleButton("G##GizmosToggle",  m_GizmosEnabled, "Gizmos",  btnSize + padding);
            DrawToggleButton("O##OutlineToggle", m_OutlineEnabled, "Outline", (btnSize + spacing) * 2 + padding - spacing);
        }

        if (m_GizmosEnabled) {
            VE::GizmoRenderer::BeginScene(m_FrameVP, vpX, vpY, vpW, vpH, m_Camera.GetMode());
            VE::GizmoRenderer::DrawGrid(20.0f, 1.0f);

            // Draw point light gizmos for all point lights
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

            // Draw spot light gizmos for all spot lights
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
                    displayAxis = VE::GizmoRenderer::HitTestTranslationGizmo(
                        worldPos, io.MousePos.x, io.MousePos.y, 12.0f, worldRot);
                }
                VE::GizmoRenderer::DrawTranslationGizmo(m_SelectedEntity, displayAxis, worldMat);
            }

            VE::GizmoRenderer::EndScene();
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

        // Save scene snapshot
        VE::SceneSerializer serializer(m_Scene);
        m_SceneSnapshot = serializer.SerializeToString();

        m_Scene->StartPhysics();
        VE::ScriptEngine::SetActiveScene(m_Scene.get());
        m_Scene->StartScripts();
        m_Scene->StartAnimations();
        m_Scene->StartSpriteAnimations();
        m_Scene->StartAudio();
        m_Scene->StartParticles();
        m_PlayMode = true;
        m_CommandHistory.Clear();
        VE_INFO("Entered Play mode");
    }

    void ExitPlayMode() {
        if (!m_PlayMode) return;
        m_Paused = false;
        m_StepOneFrame = false;

        m_Scene->StopParticles();
        m_Scene->StopAudio();
        m_Scene->StopSpriteAnimations();
        m_Scene->StopAnimations();
        m_Scene->StopScripts();
        m_Scene->StopPhysics();

        // Restore scene from snapshot
        m_Scene = std::make_shared<VE::Scene>();
        m_SelectedEntity = {};
        VE::SceneSerializer serializer(m_Scene);
        serializer.DeserializeFromString(m_SceneSnapshot);
        m_SceneSnapshot.clear();

        m_PlayMode = false;
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
        for (auto entityID : roots)
            DrawEntityNode(entityID);

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
                        m_SelectedEntity = m_Scene->CreateEntity("GameObject");
                    });
                ImGui::Separator();
                if (ImGui::MenuItem("Triangle"))
                    m_CommandHistory.Execute("Create Triangle", [this]() {
                        auto e = m_Scene->CreateEntity("Triangle");
                        auto& mr = e.AddComponent<VE::MeshRendererComponent>();
                        mr.Mesh = VE::MeshLibrary::GetTriangle();
                        mr.Mat = VE::MaterialLibrary::Get("Default");
                        m_SelectedEntity = e;
                    });
                if (ImGui::MenuItem("Quad"))
                    m_CommandHistory.Execute("Create Quad", [this]() {
                        auto e = m_Scene->CreateEntity("Quad");
                        auto& mr = e.AddComponent<VE::MeshRendererComponent>();
                        mr.Mesh = VE::MeshLibrary::GetQuad();
                        mr.Mat = VE::MaterialLibrary::Get("Default");
                        m_SelectedEntity = e;
                    });
                if (ImGui::MenuItem("Cube"))
                    m_CommandHistory.Execute("Create Cube", [this]() {
                        auto e = m_Scene->CreateEntity("Cube");
                        auto& mr = e.AddComponent<VE::MeshRendererComponent>();
                        mr.Mesh = VE::MeshLibrary::GetCube();
                        mr.Mat = VE::MaterialLibrary::Get("Lit");
                        m_SelectedEntity = e;
                    });
                if (ImGui::MenuItem("Sphere"))
                    m_CommandHistory.Execute("Create Sphere", [this]() {
                        auto e = m_Scene->CreateEntity("Sphere");
                        auto& mr = e.AddComponent<VE::MeshRendererComponent>();
                        mr.Mesh = VE::MeshLibrary::GetSphere();
                        mr.Mat = VE::MaterialLibrary::Get("Lit");
                        mr.LocalBounds = VE::MeshLibrary::GetMeshAABB(3);
                        m_SelectedEntity = e;
                    });
                ImGui::Separator();
                if (ImGui::MenuItem("Directional Light"))
                    m_CommandHistory.Execute("Create Light", [this]() {
                        auto e = m_Scene->CreateEntity("Directional Light");
                        e.AddComponent<VE::DirectionalLightComponent>();
                        m_SelectedEntity = e;
                    });
                if (ImGui::MenuItem("Point Light"))
                    m_CommandHistory.Execute("Create Point Light", [this]() {
                        auto e = m_Scene->CreateEntity("Point Light");
                        e.AddComponent<VE::PointLightComponent>();
                        m_SelectedEntity = e;
                    });
                if (ImGui::MenuItem("Spot Light"))
                    m_CommandHistory.Execute("Create Spot Light", [this]() {
                        auto e = m_Scene->CreateEntity("Spot Light");
                        e.AddComponent<VE::SpotLightComponent>();
                        m_SelectedEntity = e;
                    });
                if (ImGui::MenuItem("Camera"))
                    m_CommandHistory.Execute("Create Camera", [this]() {
                        auto e = m_Scene->CreateEntity("Camera");
                        e.AddComponent<VE::CameraComponent>();
                        m_SelectedEntity = e;
                    });
                ImGui::EndMenu();
            }
            ImGui::EndPopup();
        }

        if (m_SelectedEntity && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            m_CommandHistory.Execute("Delete Entity", [this]() {
                m_Scene->DestroyEntity(m_SelectedEntity);
                m_SelectedEntity = {};
            });
        }
        ImGui::End();
    }

    void DrawEntityNode(entt::entity entityID) {
        auto& registry = m_Scene->GetRegistry();
        if (!registry.valid(entityID)) return;

        auto& tag = registry.get<VE::TagComponent>(entityID);
        auto& rel = registry.get<VE::RelationshipComponent>(entityID);

        bool isSelected = (m_SelectedEntity.GetHandle() == entityID);
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
            m_SelectedEntity = VE::Entity(entityID, &*m_Scene);
            m_SelectedAssetPath.clear(); // deselect asset
        }

        // Right-click context menu on entity node
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Delete")) {
                m_CommandHistory.Execute("Delete Entity", [this, entityID]() {
                    m_Scene->DestroyEntity(VE::Entity(entityID, &*m_Scene));
                    if (m_SelectedEntity.GetHandle() == entityID)
                        m_SelectedEntity = {};
                });
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
                        // Auto-detect lit status: "Lit" shader or shaders with lighting uniforms
                        m_InspectedMaterial->SetLit(name == "Lit");
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

        // Material properties editor
        for (auto& prop : m_InspectedMaterial->GetProperties()) {
            ImGui::PushID(prop.Name.c_str());
            switch (prop.Type) {
                case VE::MaterialPropertyType::Float:
                    ImGui::DragFloat(prop.Name.c_str(), &prop.FloatValue, 0.01f);
                    break;
                case VE::MaterialPropertyType::Int:
                    ImGui::DragInt(prop.Name.c_str(), &prop.IntValue);
                    break;
                case VE::MaterialPropertyType::Vec3:
                    ImGui::ColorEdit3(prop.Name.c_str(), &prop.Vec3Value.x);
                    break;
                case VE::MaterialPropertyType::Vec4:
                    ImGui::ColorEdit4(prop.Name.c_str(), &prop.Vec4Value.x);
                    break;
                case VE::MaterialPropertyType::Texture2D: {
                    ImGui::Text("%s: %s", prop.Name.c_str(),
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

    void DrawMeshAssetInspector() {
        std::string absPath = m_AssetDatabase.GetAbsolutePath(m_SelectedAssetPath);
        std::string metaPath = absPath + ".meta";

        // Load settings if asset changed
        if (m_InspectedMeshPath != absPath) {
            m_InspectedMeshPath = absPath;
            m_InspectedMeshSettings = VE::FBXImporter::LoadSettings(metaPath);
            m_MeshSettingsDirty = false;

            // If no cached info, do a quick import to populate counts
            if (m_InspectedMeshSettings.VertexCount == 0) {
                VE::FBXImportSettings probe = m_InspectedMeshSettings;
                auto mesh = VE::FBXImporter::Import(absPath, probe);
                if (mesh) {
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
                m_InspectedMeshSettings.VertexCount   = newSettings.VertexCount;
                m_InspectedMeshSettings.TriangleCount  = newSettings.TriangleCount;
                m_InspectedMeshSettings.SubMeshCount   = newSettings.SubMeshCount;
                m_InspectedMeshSettings.BoneCount      = newSettings.BoneCount;
                m_InspectedMeshSettings.ClipCount      = newSettings.ClipCount;

                VE::FBXImporter::SaveSettings(metaPath, m_InspectedMeshSettings);
                VE::MeshImporter::InvalidateCache(absPath);
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

        if (!m_SelectedEntity) {
            ImGui::TextDisabled("No selection");
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
                ImGui::DragFloat3("Direction", dl.Direction.data(), 0.01f);
                if (ImGui::IsItemActivated()) m_CommandHistory.BeginPropertyEdit("Edit Light Direction");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_CommandHistory.EndPropertyEdit();
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
                ImGui::DragFloat3("Velocity Min", ps.VelocityMin.data(), 0.1f);
                ImGui::DragFloat3("Velocity Max", ps.VelocityMax.data(), 0.1f);
                ImGui::DragFloat3("Gravity", ps.Gravity.data(), 0.1f);
                ImGui::ColorEdit4("Start Color", ps.StartColor.data());
                ImGui::ColorEdit4("End Color", ps.EndColor.data());
                ImGui::DragFloat("Start Size", &ps.StartSize, 0.01f, 0.0f, 10.0f);
                ImGui::DragFloat("End Size", &ps.EndSize, 0.01f, 0.0f, 10.0f);
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

        if (ImGui::Button("Add Component", ImVec2(-1, 0)))
            ImGui::OpenPopup("AddComponentPopup");
        if (ImGui::BeginPopup("AddComponentPopup")) {
            bool anyAdded = false;

            // Scripts submenu — list all available scripts from DLL or ScriptProject scan
            if (!m_SelectedEntity.HasComponent<VE::ScriptComponent>()) {
                auto scriptNames = VE::ScriptEngine::IsLoaded()
                    ? VE::ScriptEngine::GetRegisteredClassNames()
                    : VE::ScriptEngine::ScanScriptClassNames();

                if (!scriptNames.empty() && ImGui::BeginMenu("Scripts")) {
                    for (auto& name : scriptNames) {
                        if (ImGui::MenuItem(name.c_str())) {
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
                        }
                    }
                    ImGui::EndMenu();
                }
                anyAdded = true;
            }
            if (!m_SelectedEntity.HasComponent<VE::MeshRendererComponent>()) {
                if (ImGui::MenuItem("Mesh Renderer"))
                    m_CommandHistory.Execute("Add Mesh Renderer", [this]() {
                        auto& mr = m_SelectedEntity.AddComponent<VE::MeshRendererComponent>();
                        mr.Mesh = VE::MeshLibrary::GetCube();
                        mr.Mat = VE::MaterialLibrary::Get("Lit");
                    });
                anyAdded = true;
            }
            if (!m_SelectedEntity.HasComponent<VE::LODGroupComponent>()) {
                if (ImGui::MenuItem("LOD Group"))
                    m_CommandHistory.Execute("Add LOD Group", [this]() {
                        m_SelectedEntity.AddComponent<VE::LODGroupComponent>();
                    });
                anyAdded = true;
            }
            if (!m_SelectedEntity.HasComponent<VE::DirectionalLightComponent>()) {
                if (ImGui::MenuItem("Directional Light"))
                    m_CommandHistory.Execute("Add Light", [this]() {
                        m_SelectedEntity.AddComponent<VE::DirectionalLightComponent>();
                    });
                anyAdded = true;
            }
            if (!m_SelectedEntity.HasComponent<VE::PointLightComponent>()) {
                if (ImGui::MenuItem("Point Light"))
                    m_CommandHistory.Execute("Add Point Light", [this]() {
                        m_SelectedEntity.AddComponent<VE::PointLightComponent>();
                    });
                anyAdded = true;
            }
            if (!m_SelectedEntity.HasComponent<VE::SpotLightComponent>()) {
                if (ImGui::MenuItem("Spot Light"))
                    m_CommandHistory.Execute("Add Spot Light", [this]() {
                        m_SelectedEntity.AddComponent<VE::SpotLightComponent>();
                    });
                anyAdded = true;
            }
            if (!m_SelectedEntity.HasComponent<VE::RigidbodyComponent>()) {
                if (ImGui::MenuItem("Rigidbody"))
                    m_CommandHistory.Execute("Add Rigidbody", [this]() {
                        m_SelectedEntity.AddComponent<VE::RigidbodyComponent>();
                        bool hasCol = m_SelectedEntity.HasComponent<VE::BoxColliderComponent>()
                            || m_SelectedEntity.HasComponent<VE::SphereColliderComponent>()
                            || m_SelectedEntity.HasComponent<VE::CapsuleColliderComponent>()
                            || m_SelectedEntity.HasComponent<VE::MeshColliderComponent>();
                        if (!hasCol)
                            m_SelectedEntity.AddComponent<VE::BoxColliderComponent>();
                    });
                anyAdded = true;
            }
            if (!m_SelectedEntity.HasComponent<VE::BoxColliderComponent>()) {
                if (ImGui::MenuItem("Box Collider"))
                    m_CommandHistory.Execute("Add Box Collider", [this]() {
                        m_SelectedEntity.AddComponent<VE::BoxColliderComponent>();
                    });
                anyAdded = true;
            }
            if (!m_SelectedEntity.HasComponent<VE::SphereColliderComponent>()) {
                if (ImGui::MenuItem("Sphere Collider"))
                    m_CommandHistory.Execute("Add Sphere Collider", [this]() {
                        m_SelectedEntity.AddComponent<VE::SphereColliderComponent>();
                    });
                anyAdded = true;
            }
            if (!m_SelectedEntity.HasComponent<VE::CapsuleColliderComponent>()) {
                if (ImGui::MenuItem("Capsule Collider"))
                    m_CommandHistory.Execute("Add Capsule Collider", [this]() {
                        m_SelectedEntity.AddComponent<VE::CapsuleColliderComponent>();
                    });
                anyAdded = true;
            }
            if (!m_SelectedEntity.HasComponent<VE::MeshColliderComponent>()) {
                if (ImGui::MenuItem("Mesh Collider"))
                    m_CommandHistory.Execute("Add Mesh Collider", [this]() {
                        m_SelectedEntity.AddComponent<VE::MeshColliderComponent>();
                    });
                anyAdded = true;
            }
            if (!m_SelectedEntity.HasComponent<VE::AnimatorComponent>()) {
                if (ImGui::MenuItem("Animator")) {
                    m_SelectedEntity.AddComponent<VE::AnimatorComponent>();
                }
                anyAdded = true;
            }
            if (!m_SelectedEntity.HasComponent<VE::CameraComponent>()) {
                if (ImGui::MenuItem("Camera"))
                    m_SelectedEntity.AddComponent<VE::CameraComponent>();
                anyAdded = true;
            }
            if (!m_SelectedEntity.HasComponent<VE::AudioSourceComponent>()) {
                if (ImGui::MenuItem("Audio Source"))
                    m_SelectedEntity.AddComponent<VE::AudioSourceComponent>();
                anyAdded = true;
            }
            if (!m_SelectedEntity.HasComponent<VE::AudioListenerComponent>()) {
                if (ImGui::MenuItem("Audio Listener"))
                    m_SelectedEntity.AddComponent<VE::AudioListenerComponent>();
                anyAdded = true;
            }
            if (!m_SelectedEntity.HasComponent<VE::SpriteRendererComponent>()) {
                if (ImGui::MenuItem("Sprite Renderer"))
                    m_SelectedEntity.AddComponent<VE::SpriteRendererComponent>();
                anyAdded = true;
            }
            if (!m_SelectedEntity.HasComponent<VE::SpriteAnimatorComponent>()) {
                if (ImGui::MenuItem("Sprite Animator"))
                    m_SelectedEntity.AddComponent<VE::SpriteAnimatorComponent>();
                anyAdded = true;
            }
            if (!m_SelectedEntity.HasComponent<VE::ParticleSystemComponent>()) {
                if (ImGui::MenuItem("Particle System"))
                    m_SelectedEntity.AddComponent<VE::ParticleSystemComponent>();
                anyAdded = true;
            }
            if (!m_SelectedEntity.HasComponent<VE::TerrainComponent>()) {
                if (ImGui::MenuItem("Terrain"))
                    m_SelectedEntity.AddComponent<VE::TerrainComponent>();
                anyAdded = true;
            }
            // UI components
            if (ImGui::BeginMenu("UI")) {
                if (!m_SelectedEntity.HasComponent<VE::UICanvasComponent>()) {
                    if (ImGui::MenuItem("UI Canvas"))
                        m_SelectedEntity.AddComponent<VE::UICanvasComponent>();
                }
                if (!m_SelectedEntity.HasComponent<VE::UIRectTransformComponent>()) {
                    if (ImGui::MenuItem("UI Rect Transform"))
                        m_SelectedEntity.AddComponent<VE::UIRectTransformComponent>();
                }
                if (!m_SelectedEntity.HasComponent<VE::UITextComponent>()) {
                    if (ImGui::MenuItem("UI Text"))
                        m_SelectedEntity.AddComponent<VE::UITextComponent>();
                }
                if (!m_SelectedEntity.HasComponent<VE::UIImageComponent>()) {
                    if (ImGui::MenuItem("UI Image"))
                        m_SelectedEntity.AddComponent<VE::UIImageComponent>();
                }
                if (!m_SelectedEntity.HasComponent<VE::UIButtonComponent>()) {
                    if (ImGui::MenuItem("UI Button"))
                        m_SelectedEntity.AddComponent<VE::UIButtonComponent>();
                }
                ImGui::EndMenu();
                anyAdded = true;
            }
            if (!anyAdded) {
                ImGui::TextDisabled("All components added");
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
                if (ImGui::Button("Build Scripts")) {
                    VE::ScriptEngine::BuildScriptProject();
                }
            }

            // Auto-load DLL on successful build
            if (buildStatus == VE::ScriptEngine::BuildStatus::Success && m_BuildAutoLoad) {
                auto dllPath = VE::ScriptEngine::GetBuildDLLOutputPath();
                if (!dllPath.empty() && std::filesystem::exists(dllPath)) {
                    VE::ScriptEngine::LoadScriptDLL(dllPath);
                    m_ScriptDLLPath = dllPath;
                    VE_INFO("Auto-loaded built DLL: {0}", dllPath);
                }
                m_BuildAutoLoad = false;
            }
            if (buildStatus == VE::ScriptEngine::BuildStatus::Building) {
                m_BuildAutoLoad = true;
            }

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

    void DrawPipelineSettingsPanel() {
        if (!m_ShowPipelineSettings) return;
        ImGui::Begin("Render Pipeline", &m_ShowPipelineSettings);

        auto& ps = m_Scene->GetPipelineSettings();

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

        if (ImGui::CollapsingHeader("Shadows (CSM)", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Enable Shadows", &ps.ShadowEnabled);
            if (ps.ShadowEnabled) {
                ImGui::SliderFloat("Shadow Bias", &ps.ShadowBias, 0.0f, 0.01f, "%.5f");
                ImGui::SliderFloat("Normal Bias", &ps.ShadowNormalBias, 0.0f, 0.1f, "%.3f");
                const char* pcfModes[] = { "Hard (1x1)", "Soft (3x3)", "Softer (5x5)" };
                ImGui::Combo("PCF Quality", &ps.ShadowPCFRadius, pcfModes, 3);
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
            ImGui::Checkbox("Enable Fog", &ps.FogEnabled);
            if (ps.FogEnabled) {
                const char* fogModes[] = { "Linear", "Exponential", "Exponential Squared" };
                ImGui::Combo("Fog Mode", &ps.FogMode, fogModes, 3);
                ImGui::ColorEdit3("Fog Color", ps.FogColor.data());
                if (ps.FogMode == 0) { // Linear
                    ImGui::DragFloat("Start Distance", &ps.FogStart, 1.0f, 0.0f, 10000.0f);
                    ImGui::DragFloat("End Distance", &ps.FogEnd, 1.0f, 0.0f, 10000.0f);
                } else {
                    ImGui::SliderFloat("Density", &ps.FogDensity, 0.001f, 0.5f, "%.4f");
                }
                ImGui::SliderFloat("Height Falloff", &ps.FogHeightFalloff, 0.0f, 1.0f, "%.3f");
                ImGui::SliderFloat("Max Opacity", &ps.FogMaxOpacity, 0.0f, 1.0f, "%.2f");
            }
        }

        if (ImGui::CollapsingHeader("Volumetric Fog")) {
            ImGui::Checkbox("Enable Volumetric Fog", &ps.VolFogEnabled);
            if (ps.VolFogEnabled) {
                ImGui::ColorEdit3("Fog Color##Vol", ps.VolFogColor.data());
                ImGui::SliderFloat("Density##Vol", &ps.VolFogDensity, 0.001f, 0.2f, "%.4f");
                ImGui::SliderFloat("Scattering (g)", &ps.VolFogScattering, 0.0f, 0.99f, "%.2f");
                ImGui::TextDisabled("0 = isotropic, 1 = forward (god rays)");
                ImGui::SliderFloat("Light Intensity##Vol", &ps.VolFogLightIntensity, 0.0f, 5.0f, "%.2f");
                ImGui::SliderInt("March Steps", &ps.VolFogSteps, 8, 128);
                ImGui::DragFloat("Max Distance##Vol", &ps.VolFogMaxDistance, 1.0f, 1.0f, 1000.0f);
                ImGui::SliderFloat("Height Falloff##Vol", &ps.VolFogHeightFalloff, 0.0f, 0.5f, "%.3f");
                ImGui::DragFloat("Base Height", &ps.VolFogBaseHeight, 0.5f, -100.0f, 100.0f);
            }
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
        const char* categories[] = { "Tags and Layers" };
        constexpr int categoryCount = 1;

        ImGui::BeginChild("##categories", ImVec2(150, 0), true);
        for (int i = 0; i < categoryCount; i++) {
            if (ImGui::Selectable(categories[i], selectedCategory == i))
                selectedCategory = i;
        }
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("##content", ImVec2(0, 0), true);

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
        m_SelectedEntity = {};

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

        // Breadcrumb navigation
        {
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

            // Refresh button
            ImGui::SameLine(ImGui::GetWindowWidth() - 80);
            if (ImGui::Button("Refresh")) {
                m_AssetDatabase.Refresh();
                ScanAndRegisterAssets();
            }
        }

        ImGui::Separator();

        // Two-column layout: folder tree | contents
        float treeWidth = 180.0f;

        // Left pane: folder tree
        ImGui::BeginChild("FolderTree", ImVec2(treeWidth, 0), true);
        DrawFolderTree("");
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
        float padding = 8.0f;
        float cellSize = 80.0f;
        float panelWidth = ImGui::GetContentRegionAvail().x;
        int columns = std::max(1, static_cast<int>(panelWidth / (cellSize + padding)));

        ImGui::Columns(columns, nullptr, false);

        for (auto& relPath : contents) {
            auto* meta = m_AssetDatabase.GetMetaByPath(relPath);
            if (!meta) continue;

            std::string filename = std::filesystem::path(relPath).filename().generic_string();

            ImGui::PushID(relPath.c_str());

            // Draw icon/thumbnail
            float thumbSize = cellSize - 16.0f;
            // Use InvisibleButton as the base interactive item (gives us an ID for popups/clicks)
            ImGui::InvisibleButton("##item", ImVec2(thumbSize, thumbSize));
            bool hovered = ImGui::IsItemHovered();
            ImVec2 itemMin = ImGui::GetItemRectMin();
            ImVec2 itemMax = ImGui::GetItemRectMax();

            // Draw icon/thumbnail over the invisible button
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (meta->Type == VE::AssetType::Texture2D) {
                std::string absPath = m_AssetDatabase.GetAbsolutePath(relPath);
                uint64_t texID = m_ThumbnailCache.GetThumbnail(absPath);
                if (texID != 0)
                    dl->AddImage((ImTextureID)texID, itemMin, itemMax);
                else
                    dl->AddRectFilled(itemMin, itemMax, IM_COL32(120, 70, 70, 255));
            } else if (meta->Type == VE::AssetType::Folder) {
                dl->AddRectFilled(itemMin, itemMax, IM_COL32(70, 70, 120, 255));
                float cx = (itemMin.x + itemMax.x) * 0.5f - 8.0f;
                float cy = (itemMin.y + itemMax.y) * 0.5f - 6.0f;
                dl->AddText(ImVec2(cx, cy), IM_COL32(255, 255, 255, 200), "DIR");
            } else if (meta->Type == VE::AssetType::Scene) {
                dl->AddRectFilled(itemMin, itemMax, IM_COL32(90, 120, 70, 255));
                float cx = (itemMin.x + itemMax.x) * 0.5f - 10.0f;
                float cy = (itemMin.y + itemMax.y) * 0.5f - 6.0f;
                dl->AddText(ImVec2(cx, cy), IM_COL32(255, 255, 255, 200), "SCN");
            } else if (meta->Type == VE::AssetType::Mesh) {
                dl->AddRectFilled(itemMin, itemMax, IM_COL32(100, 70, 130, 255));
                float cx = (itemMin.x + itemMax.x) * 0.5f - 10.0f;
                float cy = (itemMin.y + itemMax.y) * 0.5f - 6.0f;
                dl->AddText(ImVec2(cx, cy), IM_COL32(255, 255, 255, 200), "FBX");
            } else if (meta->Type == VE::AssetType::Shader) {
                dl->AddRectFilled(itemMin, itemMax, IM_COL32(130, 80, 130, 255));
                float cx = (itemMin.x + itemMax.x) * 0.5f - 10.0f;
                float cy = (itemMin.y + itemMax.y) * 0.5f - 6.0f;
                dl->AddText(ImVec2(cx, cy), IM_COL32(255, 255, 255, 200), "SHD");
            } else if (meta->Type == VE::AssetType::MaterialAsset) {
                dl->AddRectFilled(itemMin, itemMax, IM_COL32(180, 120, 50, 255));
                float cx = (itemMin.x + itemMax.x) * 0.5f - 10.0f;
                float cy = (itemMin.y + itemMax.y) * 0.5f - 6.0f;
                dl->AddText(ImVec2(cx, cy), IM_COL32(255, 255, 255, 200), "MAT");
            } else if (meta->Type == VE::AssetType::Audio) {
                dl->AddRectFilled(itemMin, itemMax, IM_COL32(70, 130, 100, 255));
                float cx = (itemMin.x + itemMax.x) * 0.5f - 10.0f;
                float cy = (itemMin.y + itemMax.y) * 0.5f - 6.0f;
                dl->AddText(ImVec2(cx, cy), IM_COL32(255, 255, 255, 200), "SND");
            } else {
                dl->AddRectFilled(itemMin, itemMax, IM_COL32(80, 80, 80, 255));
            }

            // Track item center for box selection
            ImVec2 itemCenter = { (itemMin.x + itemMax.x) * 0.5f, (itemMin.y + itemMax.y) * 0.5f };
            m_ContentItemPositions.push_back({ relPath, itemCenter });

            // Selection highlight (multi-select aware)
            bool isAssetSelected = m_SelectedAssets.count(relPath) > 0;
            if (isAssetSelected)
                dl->AddRect(itemMin, itemMax, IM_COL32(100, 180, 255, 255), 0.0f, 0, 2.5f);
            else if (hovered)
                dl->AddRect(itemMin, itemMax, IM_COL32(255, 200, 100, 200), 0.0f, 0, 2.0f);

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
                    for (auto& rp : contents) {
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
                m_SelectedEntity = {}; // deselect entity
            }

            // Double-click actions
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (meta->Type == VE::AssetType::Folder) {
                    m_BrowserCurrentDir = relPath;
                } else if (meta->Type == VE::AssetType::Scene) {
                    std::string absPath = m_AssetDatabase.GetAbsolutePath(relPath);
                    m_Scene = std::make_shared<VE::Scene>();
                    m_SelectedEntity = {};
                    VE::SceneSerializer serializer(m_Scene);
                    if (serializer.Deserialize(absPath))
                        m_CurrentScenePath = absPath;
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

            // Filename label (truncated)
            ImGui::TextWrapped("%s", filename.c_str());

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

    // ── FPS Overlay (small corner display, always on top) ───────────────

    void DrawFPSOverlay() {
        if (!m_ShowFPSOverlay) return;

        const float PAD = 10.0f;
        ImGuiIO& io = ImGui::GetIO();
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
            | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImVec2 pos(viewport->WorkPos.x + viewport->WorkSize.x - PAD,
                   viewport->WorkPos.y + PAD);
        ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.6f);

        if (ImGui::Begin("##FPSOverlay", &m_ShowFPSOverlay, flags)) {
            const auto& stats = VE::Profiler::GetCurrentStats();
            ImGui::Text("%.0f FPS", stats.FPS);
            ImGui::Text("%.2f ms", stats.FrameTimeMs);
            ImGui::Text("DC: %u  Tri: %s",
                VE::RenderCommand::GetStats().DrawCalls,
                FormatNumber(VE::RenderCommand::GetStats().Triangles).c_str());
        }
        ImGui::End();
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
    VE::Entity m_SelectedEntity;
    VE::EditorCamera m_Camera;
    glm::mat4 m_FrameVP = glm::mat4(1.0f);
    std::string m_CurrentScenePath;
    std::shared_ptr<VE::Framebuffer> m_Framebuffer;
    uint32_t m_CurrentMSAASamples = 1; // track to detect changes
    bool m_ViewportHovered = false;
    bool m_ViewportFocused = false;
    bool m_GizmosEnabled   = true;
    bool m_OutlineEnabled  = true;
    float m_SceneVpX = 0, m_SceneVpY = 0, m_SceneVpW = 1, m_SceneVpH = 1;

    // Gizmo drag state
    VE::GizmoAxis m_DraggingAxis = VE::GizmoAxis::None;
    glm::vec3 m_DragStartPos = { 0.0f, 0.0f, 0.0f };
    float m_DragOriginVal = 0.0f;

    std::unordered_map<uint32_t, int> m_EntityMeshIndex;

    bool m_ShowHierarchy = true;
    bool m_ShowInspector = true;
    bool m_ShowSceneInfo = true;
    bool m_ShowPipelineSettings = false;
    bool m_ShowContentBrowser = true;
    bool m_ShowScripting = false;
    bool m_ShowProjectSettings = false;
    bool m_ShowInputSettings = false;
    bool m_ShowBuildPanel = false;
    bool m_ShowProfiler = false;
    bool m_ShowFPSOverlay = false;

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

    // Scripting
    std::string m_ScriptDLLPath;
    bool m_BuildAutoLoad = false;

    // Asset management
    VE::AssetDatabase m_AssetDatabase;
    VE::ThumbnailCache m_ThumbnailCache;
    std::string m_BrowserCurrentDir; // relative to Assets/
    std::string m_SelectedAssetPath; // primary selected asset in Content Browser
    std::set<std::string> m_SelectedAssets; // multi-select set
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
};

int main() {
    Sandbox app;
    app.Run();
    return 0;
}
