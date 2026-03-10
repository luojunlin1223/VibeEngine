#include <VibeEngine/VibeEngine.h>
#include <GLFW/glfw3.h>
#include <limits>

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
    }

    ~Sandbox() override {
        VE_INFO("Sandbox application destroyed");
    }

protected:
    void OnUpdate() override {
        m_Scene->OnUpdate();

        int fbW = 0, fbH = 0;
        glfwGetFramebufferSize(GetWindow().GetNativeWindow(), &fbW, &fbH);
        if (fbW > 0 && fbH > 0)
            m_Camera.SetViewportSize(static_cast<float>(fbW), static_cast<float>(fbH));
    }

    void OnRender() override {
        m_FrameVP = m_Camera.GetViewProjection();
        glm::vec3 camPos = (m_Camera.GetMode() == VE::CameraMode::Perspective3D)
            ? m_Camera.GetPosition3D()
            : glm::vec3(m_Camera.GetPosition(), 5.0f);
        m_Scene->OnRender(m_FrameVP, camPos);
    }

    void OnImGuiRender() override {
        ImGuiIO& io = ImGui::GetIO();

        // ── Gizmo drag continuation (runs even over ImGui panels) ────
        if (m_DraggingAxis != VE::GizmoAxis::None) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                auto& tc = m_SelectedEntity.GetComponent<VE::TransformComponent>();
                glm::vec3 pos(tc.Position[0], tc.Position[1], tc.Position[2]);
                float val = VE::GizmoRenderer::ProjectMouseOntoAxis(
                    m_DraggingAxis, pos, io.MousePos.x, io.MousePos.y);
                int comp = (m_DraggingAxis == VE::GizmoAxis::X) ? 0
                         : (m_DraggingAxis == VE::GizmoAxis::Y) ? 1 : 2;
                tc.Position[comp] = m_DragStartPos[comp] + (val - m_DragOriginVal);
            } else {
                m_DraggingAxis = VE::GizmoAxis::None;
            }
        }

        // ── Camera + scene interaction ───────────────────────────────
        if (!io.WantCaptureMouse && m_DraggingAxis == VE::GizmoAxis::None) {
            if (io.MouseWheel != 0.0f)
                m_Camera.OnMouseScroll(io.MouseWheel);
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
                m_Camera.OnMouseDrag(io.MouseDelta.x, io.MouseDelta.y);
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Right))
                m_Camera.OnMouseRotate(io.MouseDelta.x, io.MouseDelta.y);

            // Left-click: gizmo drag or entity select
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                VE::GizmoAxis axis = VE::GizmoAxis::None;
                if (m_SelectedEntity && m_SelectedEntity.HasComponent<VE::TransformComponent>()) {
                    auto& tc = m_SelectedEntity.GetComponent<VE::TransformComponent>();
                    glm::vec3 pos(tc.Position[0], tc.Position[1], tc.Position[2]);
                    axis = VE::GizmoRenderer::HitTestTranslationGizmo(
                        pos, io.MousePos.x, io.MousePos.y);
                }

                if (axis != VE::GizmoAxis::None) {
                    m_DraggingAxis = axis;
                    auto& tc = m_SelectedEntity.GetComponent<VE::TransformComponent>();
                    glm::vec3 pos(tc.Position[0], tc.Position[1], tc.Position[2]);
                    m_DragStartPos = { tc.Position[0], tc.Position[1], tc.Position[2] };
                    m_DragOriginVal = VE::GizmoRenderer::ProjectMouseOntoAxis(
                        axis, pos, io.MousePos.x, io.MousePos.y);
                } else {
                    m_SelectedEntity = HitTestEntities(io.MousePos.x, io.MousePos.y);
                }
            }
        }

        // ── Gizmos ───────────────────────────────────────────────────
        ImVec2 vpPos  = ImGui::GetMainViewport()->Pos;
        ImVec2 vpSize = ImGui::GetMainViewport()->Size;

        VE::GizmoRenderer::BeginScene(m_FrameVP, vpPos.x, vpPos.y,
                                       vpSize.x, vpSize.y, m_Camera.GetMode());
        VE::GizmoRenderer::DrawGrid(20.0f, 1.0f);
        if (m_SelectedEntity) {
            VE::GizmoRenderer::DrawWireframeBox(m_SelectedEntity);

            VE::GizmoAxis displayAxis = m_DraggingAxis;
            if (displayAxis == VE::GizmoAxis::None && !io.WantCaptureMouse
                && m_SelectedEntity.HasComponent<VE::TransformComponent>()) {
                auto& tc = m_SelectedEntity.GetComponent<VE::TransformComponent>();
                glm::vec3 pos(tc.Position[0], tc.Position[1], tc.Position[2]);
                displayAxis = VE::GizmoRenderer::HitTestTranslationGizmo(
                    pos, io.MousePos.x, io.MousePos.y);
            }
            VE::GizmoRenderer::DrawTranslationGizmo(m_SelectedEntity, displayAxis);
        }

        DrawMainMenuBar();
        DrawHierarchyPanel();
        DrawInspectorPanel();
        DrawSceneInfoPanel();

        if (m_ShowDemo)
            ImGui::ShowDemoWindow(&m_ShowDemo);
    }

    void OnRendererReloaded() override {
        if (VE::MeshLibrary::GetTriangle()) {
            ClearEntityGPUResources();
            VE::MeshLibrary::Shutdown();
        } else {
            VE::MeshLibrary::Init();
            RestoreEntityGPUResources();
        }
    }

private:
    // ── Scene picking ─────────────────────────────────────────────────

    VE::Entity HitTestEntities(float screenX, float screenY) {
        // For 2D mode, use world-space AABB. For 3D, use screen-space projection.
        VE::Entity best;
        float bestDist = std::numeric_limits<float>::max();

        auto view = m_Scene->GetAllEntitiesWith<VE::TransformComponent, VE::MeshRendererComponent>();
        for (auto entityID : view) {
            auto& tc = view.get<VE::TransformComponent>(entityID);
            glm::vec3 pos(tc.Position[0], tc.Position[1], tc.Position[2]);

            // Project entity center to screen
            glm::vec4 clip = m_FrameVP * glm::vec4(pos, 1.0f);
            if (clip.w <= 0.0f) continue; // behind camera
            glm::vec3 ndc = glm::vec3(clip) / clip.w;

            ImVec2 vpPos  = ImGui::GetMainViewport()->Pos;
            ImVec2 vpSize = ImGui::GetMainViewport()->Size;
            float sx = vpPos.x + (ndc.x * 0.5f + 0.5f) * vpSize.x;
            float sy = vpPos.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * vpSize.y;

            // Approximate screen-space radius from scale
            float worldRadius = std::max({tc.Scale[0], tc.Scale[1], tc.Scale[2]}) * 0.5f;
            glm::vec4 edgeClip = m_FrameVP * glm::vec4(pos + glm::vec3(worldRadius, 0, 0), 1.0f);
            float edgeSx = vpPos.x + ((edgeClip.x / edgeClip.w) * 0.5f + 0.5f) * vpSize.x;
            float screenRadius = std::abs(edgeSx - sx);
            screenRadius = std::max(screenRadius, 20.0f); // minimum clickable size

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
            mr.Material.reset();
            mr.Texture.reset(); // GPU resource, release it
        }
    }

    void RestoreEntityGPUResources() {
        auto view = m_Scene->GetAllEntitiesWith<VE::MeshRendererComponent>();
        for (auto entityID : view) {
            auto& mr = view.get<VE::MeshRendererComponent>(entityID);
            auto it = m_EntityMeshIndex.find(static_cast<uint32_t>(entityID));
            if (it != m_EntityMeshIndex.end()) {
                mr.Mesh = VE::MeshLibrary::GetMeshByIndex(it->second);
                mr.Material = VE::MeshLibrary::IsLitMesh(it->second)
                    ? VE::MeshLibrary::GetLitShader()
                    : VE::MeshLibrary::GetDefaultShader();
            }
            if (!mr.TexturePath.empty())
                mr.Texture = VE::Texture2D::Create(mr.TexturePath);
        }
        m_EntityMeshIndex.clear();
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
                    mr.Material = VE::MeshLibrary::GetDefaultShader();
                }
                if (ImGui::MenuItem("Create Quad")) {
                    auto e = m_Scene->CreateEntity("Quad");
                    auto& mr = e.AddComponent<VE::MeshRendererComponent>();
                    mr.Mesh = VE::MeshLibrary::GetQuad();
                    mr.Material = VE::MeshLibrary::GetDefaultShader();
                }
                if (ImGui::MenuItem("Create Cube")) {
                    auto e = m_Scene->CreateEntity("Cube");
                    auto& mr = e.AddComponent<VE::MeshRendererComponent>();
                    mr.Mesh = VE::MeshLibrary::GetCube();
                    mr.Material = VE::MeshLibrary::GetLitShader();
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                ImGui::MenuItem("Hierarchy", nullptr, &m_ShowHierarchy);
                ImGui::MenuItem("Inspector", nullptr, &m_ShowInspector);
                ImGui::MenuItem("Scene Info", nullptr, &m_ShowSceneInfo);
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

    // ── Hierarchy Panel ────────────────────────────────────────────────

    void DrawHierarchyPanel() {
        if (!m_ShowHierarchy) return;
        ImGui::Begin("Hierarchy", &m_ShowHierarchy);

        auto view = m_Scene->GetAllEntitiesWith<VE::TagComponent>();
        for (auto entityID : view) {
            auto& tag = view.get<VE::TagComponent>(entityID);
            bool isSelected = (m_SelectedEntity.GetHandle() == entityID);
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf
                | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
            if (isSelected) flags |= ImGuiTreeNodeFlags_Selected;

            ImGui::TreeNodeEx(
                reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<uint32_t>(entityID))),
                flags, "%s", tag.Tag.c_str());
            if (ImGui::IsItemClicked())
                m_SelectedEntity = VE::Entity(entityID, &*m_Scene);
        }

        if (ImGui::BeginPopupContextWindow("HierarchyPopup",
                ImGuiPopupFlags_NoOpenOverItems | ImGuiPopupFlags_MouseButtonRight)) {
            if (ImGui::MenuItem("Create Empty")) m_Scene->CreateEntity("GameObject");
            if (ImGui::MenuItem("Create Triangle")) {
                auto e = m_Scene->CreateEntity("Triangle");
                auto& mr = e.AddComponent<VE::MeshRendererComponent>();
                mr.Mesh = VE::MeshLibrary::GetTriangle();
                mr.Material = VE::MeshLibrary::GetDefaultShader();
            }
            if (ImGui::MenuItem("Create Quad")) {
                auto e = m_Scene->CreateEntity("Quad");
                auto& mr = e.AddComponent<VE::MeshRendererComponent>();
                mr.Mesh = VE::MeshLibrary::GetQuad();
                mr.Material = VE::MeshLibrary::GetDefaultShader();
            }
            if (ImGui::MenuItem("Create Cube")) {
                auto e = m_Scene->CreateEntity("Cube");
                auto& mr = e.AddComponent<VE::MeshRendererComponent>();
                mr.Mesh = VE::MeshLibrary::GetCube();
                mr.Material = VE::MeshLibrary::GetLitShader();
            }
            ImGui::EndPopup();
        }

        if (m_SelectedEntity && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            m_Scene->DestroyEntity(m_SelectedEntity);
            m_SelectedEntity = {};
        }
        ImGui::End();
    }

    // ── Inspector Panel ────────────────────────────────────────────────

    void DrawInspectorPanel() {
        if (!m_ShowInspector) return;
        ImGui::Begin("Inspector", &m_ShowInspector);

        if (!m_SelectedEntity) {
            ImGui::TextDisabled("No entity selected");
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
                            mr.Material = VE::MeshLibrary::IsLitMesh(i)
                                ? VE::MeshLibrary::GetLitShader()
                                : VE::MeshLibrary::GetDefaultShader();
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }

                ImGui::ColorEdit4("Color", mr.Color.data());

                // Texture picker
                ImGui::Text("Texture: %s", mr.TexturePath.empty() ? "(none)" : mr.TexturePath.c_str());
                if (ImGui::Button("Load Texture...")) {
                    static const char* texFilter =
                        "Image Files (*.png;*.jpg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0All Files\0*.*\0";
                    std::string path = VE::FileDialog::OpenFile(texFilter, GetWindow().GetNativeWindow());
                    if (!path.empty()) {
                        mr.TexturePath = path;
                        mr.Texture = VE::Texture2D::Create(path);
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Clear Texture")) {
                    mr.TexturePath.clear();
                    mr.Texture.reset();
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
            if (!m_SelectedEntity.HasComponent<VE::MeshRendererComponent>()) {
                if (ImGui::MenuItem("Mesh Renderer")) {
                    auto& mr = m_SelectedEntity.AddComponent<VE::MeshRendererComponent>();
                    mr.Mesh = VE::MeshLibrary::GetCube();
                    mr.Material = VE::MeshLibrary::GetLitShader();
                }
            } else {
                ImGui::TextDisabled("All components added");
            }
            ImGui::EndPopup();
        }
        ImGui::End();
    }

    // ── Scene Info Panel ───────────────────────────────────────────────

    void DrawSceneInfoPanel() {
        if (!m_ShowSceneInfo) return;
        ImGui::Begin("Scene Info", &m_ShowSceneInfo);
        ImGui::Text("VibeEngine v0.2.0");
        ImGui::Separator();

        // Renderer backend
        const char* apiNames[] = { "OpenGL", "Vulkan" };
        int currentAPI = (GetCurrentAPI() == VE::RendererAPI::API::OpenGL) ? 0 : 1;
        if (ImGui::Combo("Renderer", &currentAPI, apiNames, 2)) {
            RequestSwitchAPI(currentAPI == 0
                ? VE::RendererAPI::API::OpenGL : VE::RendererAPI::API::Vulkan);
        }

        // Camera mode toggle
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
            if (mr.Mesh && mr.Material) drawCalls++;
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

    // Gizmo drag state
    VE::GizmoAxis m_DraggingAxis = VE::GizmoAxis::None;
    glm::vec3 m_DragStartPos = { 0.0f, 0.0f, 0.0f };
    float m_DragOriginVal = 0.0f;

    std::unordered_map<uint32_t, int> m_EntityMeshIndex;

    bool m_ShowHierarchy = true;
    bool m_ShowInspector = true;
    bool m_ShowSceneInfo = true;
    bool m_ShowDemo = false;
};

int main() {
    Sandbox app;
    app.Run();
    return 0;
}
