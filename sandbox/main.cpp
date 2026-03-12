#include <VibeEngine/VibeEngine.h>
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
#include <filesystem>
#include <sstream>
#include <fstream>
#include <algorithm>

static const char* s_SceneFilter = "VibeEngine Scene (*.vscene)\0*.vscene\0All Files\0*.*\0";

class Sandbox : public VE::Application {
public:
    Sandbox()
        : VE::Application(VE::RendererAPI::API::OpenGL)
    {
        VE_INFO("Sandbox application created");
        VE::ScriptEngine::Init();
        VE::ScriptEngine::SetEngineIncludePath(std::string(VE_PROJECT_ROOT) + "/engine/include");

        // Auto-build script DLL on startup if scripts exist
        if (VE::ScriptEngine::HasScriptFiles()) {
            VE_INFO("Auto-building script project...");
            VE::ScriptEngine::BuildScriptProjectSync();
        }

        VE::MeshLibrary::Init();
        m_Scene = std::make_shared<VE::Scene>();
        m_Camera.SetViewportSize(1280.0f, 720.0f);
        m_AssetDatabase.Init(".");
        ScanAndRegisterAssets();

        VE::FramebufferSpec fbSpec;
        fbSpec.Width = 1280;
        fbSpec.Height = 720;
        fbSpec.HDR = true;
        m_Framebuffer = VE::Framebuffer::Create(fbSpec);
    }

    ~Sandbox() override {
        VE::ScriptEngine::Shutdown();
        VE_INFO("Sandbox application destroyed");
    }

protected:
    void OnUpdate() override {
        m_Scene->OnUpdate(m_DeltaTime);
        m_AssetDatabase.Update(m_DeltaTime);
        if (m_PlayMode)
            VE::ScriptEngine::CheckForReload();
    }

    void OnRender() override {
        m_FrameVP = m_Camera.GetViewProjection();
        glm::vec3 camPos = (m_Camera.GetMode() == VE::CameraMode::Perspective3D)
            ? m_Camera.GetPosition3D()
            : glm::vec3(m_Camera.GetPosition(), 5.0f);

        // Compute CSM shadows before main render (only in 3D perspective mode)
        // Must run before binding the scene framebuffer since it uses its own FBO
        if (m_Camera.GetMode() == VE::CameraMode::Perspective3D) {
            m_Scene->ComputeShadows(m_Camera.GetViewMatrix(),
                                    m_Camera.GetProjectionMatrix(),
                                    m_Camera.GetNearClip(),
                                    m_Camera.GetFarClip());
        }

        if (m_Framebuffer) {
            m_Framebuffer->Bind();
            VE::RenderCommand::SetClearColor(0.1f, 0.1f, 0.1f, 1.0f);
            VE::RenderCommand::Clear();
        }

        m_Scene->OnRenderSky(m_Camera.GetSkyViewProjection());
        m_Scene->OnRender(m_FrameVP, camPos);

        if (m_Framebuffer) {
            m_Framebuffer->Unbind();

            // Apply post-processing
            auto& ps = m_Scene->GetPipelineSettings();
            VE::PostProcessSettings ppSettings;
            ppSettings.Bloom = { ps.BloomEnabled, ps.BloomThreshold, ps.BloomIntensity, ps.BloomIterations };
            ppSettings.Vignette = { ps.VignetteEnabled, ps.VignetteIntensity, ps.VignetteSmoothness };
            ppSettings.Color = { ps.ColorAdjustEnabled, ps.ColorExposure, ps.ColorContrast,
                                 ps.ColorSaturation, ps.ColorFilter, ps.ColorGamma };
            ppSettings.SMH = { ps.SMHEnabled, ps.SMH_Shadows, ps.SMH_Midtones, ps.SMH_Highlights,
                               ps.SMH_ShadowStart, ps.SMH_ShadowEnd, ps.SMH_HighlightStart, ps.SMH_HighlightEnd };
            ppSettings.Curves.Enabled = ps.CurvesEnabled;
            ppSettings.Curves.Master.Points = ps.CurvesMaster;
            ppSettings.Curves.Red.Points    = ps.CurvesRed;
            ppSettings.Curves.Green.Points  = ps.CurvesGreen;
            ppSettings.Curves.Blue.Points   = ps.CurvesBlue;
            ppSettings.Tonemap = { ps.TonemapEnabled, static_cast<VE::TonemapMode>(ps.TonemapMode) };

            uint32_t sceneTex = static_cast<uint32_t>(m_Framebuffer->GetColorAttachmentID());
            m_PostProcessedTexture = m_PostProcessing.Apply(
                sceneTex, m_Framebuffer->GetWidth(), m_Framebuffer->GetHeight(), ppSettings);

            // If no effects active, Apply returns the original texture
            if (m_PostProcessedTexture == sceneTex)
                m_PostProcessedTexture = 0;
        }
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
            ImGui::SetNextWindowPos(vp->WorkPos);
            ImGui::SetNextWindowSize(vp->WorkSize);
            ImGui::SetNextWindowViewport(vp->ID);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::Begin("DockSpaceWindow", nullptr, dockFlags);
            ImGui::PopStyleVar(3);
            ImGui::DockSpace(ImGui::GetID("MainDockSpace"), ImVec2(0, 0), ImGuiDockNodeFlags_None);
            ImGui::End();
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

        if (m_ShowDemo)
            ImGui::ShowDemoWindow(&m_ShowDemo);
    }

    void OnRendererReloaded() override {
        if (VE::MeshLibrary::GetTriangle()) {
            ClearEntityGPUResources();
            VE::MeshLibrary::Shutdown();
            VE::MeshImporter::ClearCache();
            m_Framebuffer.reset();
            m_PostProcessing.Shutdown();
            m_PostProcessedTexture = 0;
        } else {
            VE::MeshLibrary::Init();
            VE::MeshImporter::ReuploadCache();
            RestoreEntityGPUResources();
            VE::FramebufferSpec fbSpec;
            fbSpec.Width = 1280;
            fbSpec.Height = 720;
            fbSpec.HDR = true;
            m_Framebuffer = VE::Framebuffer::Create(fbSpec);
        }
        m_ThumbnailCache.Clear();
    }

private:
    // ── Scene picking ─────────────────────────────────────────────────

    VE::Entity HitTestEntities(float screenX, float screenY) {
        VE::Entity best;
        float bestDist = std::numeric_limits<float>::max();

        auto view = m_Scene->GetAllEntitiesWith<VE::TransformComponent, VE::MeshRendererComponent>();
        for (auto entityID : view) {
            auto& tc = view.get<VE::TransformComponent>(entityID);
            glm::vec3 pos(tc.Position[0], tc.Position[1], tc.Position[2]);

            glm::vec4 clip = m_FrameVP * glm::vec4(pos, 1.0f);
            if (clip.w <= 0.0f) continue;
            glm::vec3 ndc = glm::vec3(clip) / clip.w;

            ImVec2 vpPos  = ImGui::GetMainViewport()->Pos;
            ImVec2 vpSize = ImGui::GetMainViewport()->Size;
            float sx = vpPos.x + (ndc.x * 0.5f + 0.5f) * vpSize.x;
            float sy = vpPos.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * vpSize.y;

            float worldRadius = std::max({tc.Scale[0], tc.Scale[1], tc.Scale[2]}) * 0.5f;
            glm::vec4 edgeClip = m_FrameVP * glm::vec4(pos + glm::vec3(worldRadius, 0, 0), 1.0f);
            float edgeSx = vpPos.x + ((edgeClip.x / edgeClip.w) * 0.5f + 0.5f) * vpSize.x;
            float screenRadius = std::abs(edgeSx - sx);
            screenRadius = std::max(screenRadius, 20.0f);

            float dx = screenX - sx, dy = screenY - sy;
            float dist = std::sqrt(dx * dx + dy * dy);
            if (dist < screenRadius && dist < bestDist) {
                bestDist = dist;
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
                if (ImGui::MenuItem("Exit"))
                    glfwSetWindowShouldClose(GetWindow().GetNativeWindow(), true);
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
                if (ImGui::MenuItem("Create Directional Light")) {
                    auto e = m_Scene->CreateEntity("Directional Light");
                    e.AddComponent<VE::DirectionalLightComponent>();
                }
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
                ImGui::Separator();
                ImGui::MenuItem("Demo Window", nullptr, &m_ShowDemo);
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
                    auto entity = m_Scene->CreateEntity(meshAsset->GetName());
                    auto& mr = entity.AddComponent<VE::MeshRendererComponent>();
                    mr.Mesh = meshAsset->VAO;
                    mr.Mat = VE::MaterialLibrary::Get("Lit");
                    mr.MeshSourcePath = path;
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

        VE::GizmoRenderer::BeginScene(m_FrameVP, vpX, vpY, vpW, vpH, m_Camera.GetMode());
        VE::GizmoRenderer::DrawGrid(20.0f, 1.0f);
        if (m_SelectedEntity && m_SelectedEntity.HasComponent<VE::TransformComponent>()) {
            glm::mat4 worldMat = m_Scene->GetWorldTransform(m_SelectedEntity.GetHandle());
            VE::GizmoRenderer::DrawWireframeBox(worldMat);

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
        m_PlayMode = true;
        VE_INFO("Entered Play mode");
    }

    void ExitPlayMode() {
        if (!m_PlayMode) return;

        m_Scene->StopScripts();
        m_Scene->StopPhysics();

        // Restore scene from snapshot
        m_Scene = std::make_shared<VE::Scene>();
        m_SelectedEntity = {};
        VE::SceneSerializer serializer(m_Scene);
        serializer.DeserializeFromString(m_SceneSnapshot);
        m_SceneSnapshot.clear();

        m_PlayMode = false;
        VE_INFO("Exited Play mode (scene restored)");
    }

    void DrawToolbar() {
        bool wasPlayMode = m_PlayMode;

        if (wasPlayMode)
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.35f, 0.15f, 0.15f, 1.0f));

        ImGui::Begin("Toolbar", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        float windowWidth = ImGui::GetContentRegionAvail().x;
        float buttonWidth = 80.0f;
        ImGui::SetCursorPosX((windowWidth - buttonWidth) * 0.5f);

        if (!m_PlayMode) {
            if (ImGui::Button("Play", ImVec2(buttonWidth, 0)))
                EnterPlayMode();
        } else {
            if (ImGui::Button("Stop", ImVec2(buttonWidth, 0)))
                ExitPlayMode();
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "PLAY MODE");
        }

        ImGui::End();

        if (wasPlayMode)
            ImGui::PopStyleColor();
    }

    // ── Hierarchy Panel ────────────────────────────────────────────────

    void DrawHierarchyPanel() {
        if (!m_ShowHierarchy) return;
        ImGui::Begin("Hierarchy", &m_ShowHierarchy);

        // Draw only root entities (no parent), children are drawn recursively
        auto view = m_Scene->GetAllEntitiesWith<VE::RelationshipComponent>();
        for (auto entityID : view) {
            auto& rel = view.get<VE::RelationshipComponent>(entityID);
            if (rel.Parent == entt::null)
                DrawEntityNode(entityID);
        }

        // Drop on empty space: unparent
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENTITY_NODE")) {
                entt::entity dropped = *static_cast<const entt::entity*>(payload->Data);
                m_Scene->RemoveParent(dropped);
            }
            ImGui::EndDragDropTarget();
        }

        if (ImGui::BeginPopupContextWindow("HierarchyPopup",
                ImGuiPopupFlags_NoOpenOverItems | ImGuiPopupFlags_MouseButtonRight)) {
            if (ImGui::MenuItem("Create Empty")) m_Scene->CreateEntity("GameObject");
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
            if (ImGui::MenuItem("Create Directional Light")) {
                auto e = m_Scene->CreateEntity("Directional Light");
                e.AddComponent<VE::DirectionalLightComponent>();
            }
            ImGui::EndPopup();
        }

        if (m_SelectedEntity && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            m_Scene->DestroyEntity(m_SelectedEntity);
            m_SelectedEntity = {};
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

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (isSelected) flags |= ImGuiTreeNodeFlags_Selected;
        if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf;

        bool opened = ImGui::TreeNodeEx(
            reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<uint32_t>(entityID))),
            flags, "%s", tag.Tag.c_str());

        if (ImGui::IsItemClicked()) {
            m_SelectedEntity = VE::Entity(entityID, &*m_Scene);
            m_SelectedAssetPath.clear(); // deselect asset
        }

        // Right-click context menu on entity node
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Delete")) {
                m_Scene->DestroyEntity(VE::Entity(entityID, &*m_Scene));
                if (m_SelectedEntity.GetHandle() == entityID)
                    m_SelectedEntity = {};
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
                if (dropped != entityID)
                    m_Scene->SetParent(dropped, entityID);
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
            char buffer[256];
            strncpy(buffer, tag.Tag.c_str(), sizeof(buffer) - 1);
            buffer[sizeof(buffer) - 1] = '\0';
            if (ImGui::InputText("Name", buffer, sizeof(buffer)))
                tag.Tag = buffer;
            ImGui::Separator();
        }

        if (m_SelectedEntity.HasComponent<VE::TransformComponent>()) {
            if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
                auto& tc = m_SelectedEntity.GetComponent<VE::TransformComponent>();
                ImGui::DragFloat3("Position", tc.Position.data(), 0.1f);
                ImGui::DragFloat3("Rotation", tc.Rotation.data(), 1.0f);
                ImGui::DragFloat3("Scale",    tc.Scale.data(),    0.1f, 0.01f, 100.0f);
            }
            ImGui::Separator();
        }

        if (m_SelectedEntity.HasComponent<VE::DirectionalLightComponent>()) {
            bool removeLight = false;
            if (ImGui::CollapsingHeader("Directional Light", ImGuiTreeNodeFlags_DefaultOpen)) {
                auto& dl = m_SelectedEntity.GetComponent<VE::DirectionalLightComponent>();
                ImGui::DragFloat3("Direction", dl.Direction.data(), 0.01f);
                ImGui::ColorEdit3("Light Color", dl.Color.data());
                ImGui::DragFloat("Intensity", &dl.Intensity, 0.01f, 0.0f, 10.0f);
                if (ImGui::Button("Remove Light"))
                    removeLight = true;
            }
            if (removeLight)
                m_SelectedEntity.RemoveComponent<VE::DirectionalLightComponent>();
            ImGui::Separator();
        }

        if (m_SelectedEntity.HasComponent<VE::RigidbodyComponent>()) {
            bool removeRB = false;
            if (ImGui::CollapsingHeader("Rigidbody", ImGuiTreeNodeFlags_DefaultOpen)) {
                auto& rb = m_SelectedEntity.GetComponent<VE::RigidbodyComponent>();
                const char* bodyTypes[] = { "Static", "Kinematic", "Dynamic" };
                int currentType = static_cast<int>(rb.Type);
                if (ImGui::Combo("Body Type", &currentType, bodyTypes, 3))
                    rb.Type = static_cast<VE::BodyType>(currentType);
                if (rb.Type == VE::BodyType::Dynamic) {
                    ImGui::DragFloat("Mass", &rb.Mass, 0.1f, 0.01f, 1000.0f);
                }
                ImGui::DragFloat("Linear Damping", &rb.LinearDamping, 0.01f, 0.0f, 10.0f);
                ImGui::DragFloat("Angular Damping", &rb.AngularDamping, 0.01f, 0.0f, 10.0f);
                ImGui::DragFloat("Restitution", &rb.Restitution, 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("Friction", &rb.Friction, 0.01f, 0.0f, 2.0f);
                ImGui::Checkbox("Use Gravity", &rb.UseGravity);
                if (!m_SelectedEntity.HasComponent<VE::ColliderComponent>())
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Needs Collider component to work!");
                if (ImGui::Button("Remove Rigidbody"))
                    removeRB = true;
            }
            if (removeRB)
                m_SelectedEntity.RemoveComponent<VE::RigidbodyComponent>();
            ImGui::Separator();
        }

        if (m_SelectedEntity.HasComponent<VE::ColliderComponent>()) {
            bool removeCol = false;
            if (ImGui::CollapsingHeader("Collider", ImGuiTreeNodeFlags_DefaultOpen)) {
                auto& col = m_SelectedEntity.GetComponent<VE::ColliderComponent>();
                const char* shapes[] = { "Box", "Sphere", "Capsule" };
                int currentShape = static_cast<int>(col.Shape);
                if (ImGui::Combo("Shape", &currentShape, shapes, 3))
                    col.Shape = static_cast<VE::ColliderShape>(currentShape);
                if (col.Shape == VE::ColliderShape::Box)
                    ImGui::DragFloat3("Size", col.Size.data(), 0.1f, 0.01f, 100.0f);
                else if (col.Shape == VE::ColliderShape::Sphere)
                    ImGui::DragFloat("Diameter", &col.Size[0], 0.1f, 0.01f, 100.0f);
                else {
                    ImGui::DragFloat("Diameter", &col.Size[0], 0.1f, 0.01f, 100.0f);
                    ImGui::DragFloat("Height", &col.Size[1], 0.1f, 0.01f, 100.0f);
                }
                ImGui::DragFloat3("Offset", col.Offset.data(), 0.1f);
                if (ImGui::Button("Remove Collider"))
                    removeCol = true;
            }
            if (removeCol)
                m_SelectedEntity.RemoveComponent<VE::ColliderComponent>();
            ImGui::Separator();
        }

        if (m_SelectedEntity.HasComponent<VE::ScriptComponent>()) {
            auto& sc = m_SelectedEntity.GetComponent<VE::ScriptComponent>();
            bool removeScript = false;
            std::string headerLabel = sc.ClassName.empty() ? "Script" : sc.ClassName;
            if (ImGui::CollapsingHeader(headerLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
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

                if (ImGui::Button("Remove Script"))
                    removeScript = true;
            }
            if (removeScript)
                m_SelectedEntity.RemoveComponent<VE::ScriptComponent>();
            ImGui::Separator();
        }

        if (m_SelectedEntity.HasComponent<VE::MeshRendererComponent>()) {
            bool removeComponent = false;
            if (ImGui::CollapsingHeader("Mesh Renderer", ImGuiTreeNodeFlags_DefaultOpen)) {
                auto& mr = m_SelectedEntity.GetComponent<VE::MeshRendererComponent>();

                // ── Mesh Object Field (Unity-style) ──
                DrawObjectField("Mesh", mr);

                // ── Material Object Field (Unity-style) ──
                DrawMaterialObjectField("Material", mr);

                ImGui::Checkbox("Cast Shadows", &mr.CastShadows);

                // Per-entity material property overrides
                if (mr.Mat) {
                    // Ensure overrides are populated from material defaults
                    if (mr.MaterialOverrides.empty()) {
                        for (const auto& prop : mr.Mat->GetProperties()) {
                            if (prop.Name == "u_MainTex") continue; // texture handled separately
                            mr.MaterialOverrides.push_back(prop);
                        }
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
                                    ImGui::Text("%s: %s", label,
                                        ov.TexturePath.empty() ? "(none)" : ov.TexturePath.c_str());
                                    ImGui::SameLine();
                                    if (ImGui::SmallButton("Load")) {
                                        static const char* texFilter =
                                            "Image Files (*.png;*.jpg;*.hdr)\0*.png;*.jpg;*.jpeg;*.hdr\0All Files\0*.*\0";
                                        std::string path = VE::FileDialog::OpenFile(texFilter, GetWindow().GetNativeWindow());
                                        if (!path.empty()) {
                                            ov.TexturePath = path;
                                            ov.TextureRef = VE::Texture2D::Create(path);
                                        }
                                    }
                                    ImGui::SameLine();
                                    if (ImGui::SmallButton("Clear")) {
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

                if (ImGui::Button("Remove Component"))
                    removeComponent = true;
            }
            if (removeComponent)
                m_SelectedEntity.RemoveComponent<VE::MeshRendererComponent>();
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
                            auto& sc = m_SelectedEntity.AddComponent<VE::ScriptComponent>();
                            sc.ClassName = name;
                            // Initialize properties with defaults
                            auto cachedProps = VE::ScriptEngine::GetScriptProperties(name);
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
                        }
                    }
                    ImGui::EndMenu();
                }
                anyAdded = true;
            }
            if (!m_SelectedEntity.HasComponent<VE::MeshRendererComponent>()) {
                if (ImGui::MenuItem("Mesh Renderer")) {
                    auto& mr = m_SelectedEntity.AddComponent<VE::MeshRendererComponent>();
                    mr.Mesh = VE::MeshLibrary::GetCube();
                    mr.Mat = VE::MaterialLibrary::Get("Lit");
                }
                anyAdded = true;
            }
            if (!m_SelectedEntity.HasComponent<VE::DirectionalLightComponent>()) {
                if (ImGui::MenuItem("Directional Light")) {
                    m_SelectedEntity.AddComponent<VE::DirectionalLightComponent>();
                }
                anyAdded = true;
            }
            if (!m_SelectedEntity.HasComponent<VE::RigidbodyComponent>()) {
                if (ImGui::MenuItem("Rigidbody")) {
                    m_SelectedEntity.AddComponent<VE::RigidbodyComponent>();
                    if (!m_SelectedEntity.HasComponent<VE::ColliderComponent>())
                        m_SelectedEntity.AddComponent<VE::ColliderComponent>();
                }
                anyAdded = true;
            }
            if (!m_SelectedEntity.HasComponent<VE::ColliderComponent>()) {
                if (ImGui::MenuItem("Collider")) {
                    m_SelectedEntity.AddComponent<VE::ColliderComponent>();
                }
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

                ImGui::Text("Sky Texture: %s",
                    ps.SkyTexturePath.empty() ? "(none - gradient)" : ps.SkyTexturePath.c_str());
                if (ImGui::Button("Load Sky Texture...")) {
                    static const char* texFilter =
                        "Image Files (*.png;*.jpg;*.hdr)\0*.png;*.jpg;*.jpeg;*.hdr\0All Files\0*.*\0";
                    std::string path = VE::FileDialog::OpenFile(texFilter, GetWindow().GetNativeWindow());
                    if (!path.empty()) {
                        ps.SkyTexturePath = path;
                        ps.SkyTexture = VE::Texture2D::Create(path);
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Clear##SkyTex")) {
                    ps.SkyTexturePath.clear();
                    ps.SkyTexture.reset();
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
            bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
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
            } else {
                dl->AddRectFilled(itemMin, itemMax, IM_COL32(80, 80, 80, 255));
            }

            // Selection highlight
            bool isAssetSelected = (m_SelectedAssetPath == relPath);
            if (isAssetSelected)
                dl->AddRect(itemMin, itemMax, IM_COL32(100, 180, 255, 255), 0.0f, 0, 2.5f);
            else if (hovered)
                dl->AddRect(itemMin, itemMax, IM_COL32(255, 200, 100, 200), 0.0f, 0, 2.0f);

            // Single-click: select asset in inspector
            if (clicked) {
                m_SelectedAssetPath = relPath;
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

            // Drag-drop source for material files
            if (meta->Type == VE::AssetType::MaterialAsset) {
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                    std::string absPath = m_AssetDatabase.GetAbsolutePath(relPath);
                    ImGui::SetDragDropPayload("ASSET_MATERIAL", absPath.c_str(), absPath.size() + 1);
                    ImGui::Text("%s", filename.c_str());
                    ImGui::EndDragDropSource();
                }
            }

            // Right-click context menu on item (works because InvisibleButton has an ID)
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Delete")) {
                    m_AssetDatabase.DeleteAsset(relPath);
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
        ImGui::Text("VibeEngine v0.2.0");
        ImGui::Separator();

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

        int drawCalls = 0;
        auto view = m_Scene->GetAllEntitiesWith<VE::MeshRendererComponent>();
        for (auto e : view) {
            auto& mr = view.get<VE::MeshRendererComponent>(e);
            if (mr.Mesh && mr.Mat) drawCalls++;
        }
        ImGui::Text("Entities: %d", static_cast<int>(m_Scene->GetRegistry().storage<entt::entity>().size()));
        ImGui::Text("Draw calls: %d", drawCalls);

        ImGui::Separator();
        ImGui::Text("Scene: %s", m_CurrentScenePath.empty() ? "(unsaved)" : m_CurrentScenePath.c_str());

        ImGui::Separator();
        if (m_Camera.GetMode() == VE::CameraMode::Perspective3D) {
            auto p = m_Camera.GetPosition3D();
            ImGui::Text("Camera: (%.1f, %.1f, %.1f) dist=%.2f", p.x, p.y, p.z, m_Camera.GetDistance());
        } else {
            ImGui::Text("Camera: (%.1f, %.1f) zoom=%.2f",
                m_Camera.GetPosition().x, m_Camera.GetPosition().y, m_Camera.GetZoom());
        }
        ImGui::Text("FPS: %.1f (%.3f ms)", ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);
        ImGui::End();
    }

private:
    std::shared_ptr<VE::Scene> m_Scene;
    VE::Entity m_SelectedEntity;
    VE::EditorCamera m_Camera;
    glm::mat4 m_FrameVP = glm::mat4(1.0f);
    std::string m_CurrentScenePath;
    std::shared_ptr<VE::Framebuffer> m_Framebuffer;
    bool m_ViewportHovered = false;
    bool m_ViewportFocused = false;

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
    bool m_ShowDemo = false;

    // Play mode
    bool m_PlayMode = false;
    std::string m_SceneSnapshot;

    // Scripting
    std::string m_ScriptDLLPath;
    bool m_BuildAutoLoad = false;

    // Asset management
    VE::AssetDatabase m_AssetDatabase;
    VE::ThumbnailCache m_ThumbnailCache;
    std::string m_BrowserCurrentDir; // relative to Assets/
    std::string m_SelectedAssetPath; // selected asset in Content Browser
    std::shared_ptr<VE::Material> m_InspectedMaterial;
    std::string m_InspectedMaterialPath;

    // Mesh asset inspector
    VE::FBXImportSettings m_InspectedMeshSettings;
    std::string m_InspectedMeshPath;
    bool m_MeshSettingsDirty = false;

    // Post-processing
    VE::PostProcessing m_PostProcessing;
    uint32_t m_PostProcessedTexture = 0;
};

int main() {
    Sandbox app;
    app.Run();
    return 0;
}
