#include <VibeEngine/VibeEngine.h>
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
#include <filesystem>
#include <sstream>
#include <fstream>

static const char* s_SceneFilter = "VibeEngine Scene (*.vscene)\0*.vscene\0All Files\0*.*\0";

class Sandbox : public VE::Application {
public:
    Sandbox()
        : VE::Application(VE::RendererAPI::API::OpenGL)
    {
        VE_INFO("Sandbox application created");
        VE::MeshLibrary::Init();
        m_Scene = std::make_shared<VE::Scene>();
        m_Camera.SetViewportSize(1280.0f, 720.0f);
        m_AssetDatabase.Init(".");

        VE::FramebufferSpec fbSpec;
        fbSpec.Width = 1280;
        fbSpec.Height = 720;
        m_Framebuffer = VE::Framebuffer::Create(fbSpec);
    }

    ~Sandbox() override {
        VE_INFO("Sandbox application destroyed");
    }

protected:
    void OnUpdate() override {
        m_Scene->OnUpdate(m_DeltaTime);
        m_AssetDatabase.Update(m_DeltaTime);
    }

    void OnRender() override {
        m_FrameVP = m_Camera.GetViewProjection();
        glm::vec3 camPos = (m_Camera.GetMode() == VE::CameraMode::Perspective3D)
            ? m_Camera.GetPosition3D()
            : glm::vec3(m_Camera.GetPosition(), 5.0f);

        if (m_Framebuffer) {
            m_Framebuffer->Bind();
            VE::RenderCommand::SetClearColor(0.1f, 0.1f, 0.1f, 1.0f);
            VE::RenderCommand::Clear();
        }

        m_Scene->OnRenderSky(m_Camera.GetSkyViewProjection());
        m_Scene->OnRender(m_FrameVP, camPos);

        if (m_Framebuffer)
            m_Framebuffer->Unbind();
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
        } else {
            VE::MeshLibrary::Init();
            VE::MeshImporter::ReuploadCache();
            RestoreEntityGPUResources();
            VE::FramebufferSpec fbSpec;
            fbSpec.Width = 1280;
            fbSpec.Height = 720;
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

        // Find a unique name
        std::string name = baseName;
        std::string relPath = relDir.empty() ? name + ext : relDir + "/" + name + ext;
        std::string absPath = (std::filesystem::path(assetsRoot) / relPath).generic_string();
        int counter = 1;
        while (std::filesystem::exists(absPath)) {
            name = baseName + " " + std::to_string(counter++);
            relPath = relDir.empty() ? name + ext : relDir + "/" + name + ext;
            absPath = (std::filesystem::path(assetsRoot) / relPath).generic_string();
        }

        if (ext == ".vmat") {
            // Create a default material file
            auto mat = VE::Material::Create(name, VE::MeshLibrary::GetLitShader());
            mat->SetLit(true);
            mat->SetVec4("u_EntityColor", glm::vec4(1.0f));
            mat->Save(absPath);
            VE::MaterialLibrary::Register(mat);
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

        m_AssetDatabase.Refresh();
        VE_INFO("Created asset: {0}", relPath);
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

        // Display the framebuffer texture
        if (m_Framebuffer) {
            uint64_t texID = m_Framebuffer->GetColorAttachmentID();
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
                    auto entity = m_Scene->CreateEntity(meshAsset->Name);
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

        ImGui::End();
        ImGui::PopStyleVar();
    }

    // ── Toolbar ────────────────────────────────────────────────────────

    void EnterPlayMode() {
        if (m_PlayMode) return;

        // Save scene snapshot
        VE::SceneSerializer serializer(m_Scene);
        m_SceneSnapshot = serializer.SerializeToString();

        m_Scene->StartPhysics();
        m_PlayMode = true;
        VE_INFO("Entered Play mode");
    }

    void ExitPlayMode() {
        if (!m_PlayMode) return;

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
            ImGui::Text("Type: Shader (.glsl)");
        } else if (meta->Type == VE::AssetType::Mesh) {
            ImGui::Text("Type: Mesh");
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

        // Shader picker
        {
            const char* currentShaderName = "None";
            auto shader = m_InspectedMaterial->GetShader();
            // Find shader name by comparing pointers with built-in shaders
            if (shader == VE::MeshLibrary::GetDefaultShader()) currentShaderName = "Default";
            else if (shader == VE::MeshLibrary::GetLitShader()) currentShaderName = "Lit";
            else if (shader == VE::MeshLibrary::GetSkyShader()) currentShaderName = "Sky";

            if (ImGui::BeginCombo("Shader", currentShaderName)) {
                if (ImGui::Selectable("Default", shader == VE::MeshLibrary::GetDefaultShader())) {
                    m_InspectedMaterial->SetShader(VE::MeshLibrary::GetDefaultShader());
                    m_InspectedMaterial->SetLit(false);
                }
                if (ImGui::Selectable("Lit", shader == VE::MeshLibrary::GetLitShader())) {
                    m_InspectedMaterial->SetShader(VE::MeshLibrary::GetLitShader());
                    m_InspectedMaterial->SetLit(true);
                }
                if (ImGui::Selectable("Sky", shader == VE::MeshLibrary::GetSkyShader())) {
                    m_InspectedMaterial->SetShader(VE::MeshLibrary::GetSkyShader());
                    m_InspectedMaterial->SetLit(false);
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

        if (m_SelectedEntity.HasComponent<VE::MeshRendererComponent>()) {
            bool removeComponent = false;
            if (ImGui::CollapsingHeader("Mesh Renderer", ImGuiTreeNodeFlags_DefaultOpen)) {
                auto& mr = m_SelectedEntity.GetComponent<VE::MeshRendererComponent>();

                const char* currentMesh = "None";
                int currentIndex = -1;
                for (int i = 0; i < VE::MeshLibrary::GetMeshCount(); i++) {
                    if (VE::MeshLibrary::GetMeshByIndex(i) == mr.Mesh) {
                        currentMesh = VE::MeshLibrary::GetMeshName(i);
                        currentIndex = i;
                        break;
                    }
                }

                if (ImGui::BeginCombo("Mesh", currentMesh)) {
                    for (int i = 0; i < VE::MeshLibrary::GetMeshCount(); i++) {
                        bool selected = (i == currentIndex);
                        if (ImGui::Selectable(VE::MeshLibrary::GetMeshName(i), selected)) {
                            mr.Mesh = VE::MeshLibrary::GetMeshByIndex(i);
                            mr.Mat = VE::MeshLibrary::IsLitMesh(i)
                                ? VE::MaterialLibrary::Get("Lit")
                                : VE::MaterialLibrary::Get("Default");
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }

                // Material picker
                {
                    const char* currentMatName = mr.Mat ? mr.Mat->GetName().c_str() : "None";
                    if (ImGui::BeginCombo("Material", currentMatName)) {
                        auto matNames = VE::MaterialLibrary::GetAllNames();
                        for (auto& name : matNames) {
                            bool selected = mr.Mat && mr.Mat->GetName() == name;
                            if (ImGui::Selectable(name.c_str(), selected))
                                mr.Mat = VE::MaterialLibrary::Get(name);
                            if (selected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                }

                ImGui::ColorEdit4("Color", mr.Color.data());

                // Inline material properties
                if (mr.Mat) {
                    ImGui::Separator();
                    ImGui::Text("Material: %s", mr.Mat->GetName().c_str());
                    for (auto& prop : mr.Mat->GetProperties()) {
                        switch (prop.Type) {
                            case VE::MaterialPropertyType::Float:
                                ImGui::DragFloat(prop.Name.c_str(), &prop.FloatValue, 0.01f);
                                break;
                            case VE::MaterialPropertyType::Vec3:
                                ImGui::ColorEdit3(prop.Name.c_str(), &prop.Vec3Value.x);
                                break;
                            case VE::MaterialPropertyType::Vec4:
                                ImGui::ColorEdit4(prop.Name.c_str(), &prop.Vec4Value.x);
                                break;
                            case VE::MaterialPropertyType::Texture2D:
                                ImGui::Text("Tex: %s", prop.TexturePath.empty() ? "(none)" : prop.TexturePath.c_str());
                                break;
                            default: break;
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

        ImGui::End();
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
            if (ImGui::Button("Refresh"))
                m_AssetDatabase.Refresh();
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
                        auto entity = m_Scene->CreateEntity(meshAsset->Name);
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
    bool m_ShowDemo = false;

    // Play mode
    bool m_PlayMode = false;
    std::string m_SceneSnapshot;

    // Asset management
    VE::AssetDatabase m_AssetDatabase;
    VE::ThumbnailCache m_ThumbnailCache;
    std::string m_BrowserCurrentDir; // relative to Assets/
    std::string m_SelectedAssetPath; // selected asset in Content Browser
    std::shared_ptr<VE::Material> m_InspectedMaterial;
    std::string m_InspectedMaterialPath;
};

int main() {
    Sandbox app;
    app.Run();
    return 0;
}
