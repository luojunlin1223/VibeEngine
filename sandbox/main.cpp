#include <VibeEngine/VibeEngine.h>
#include <GLFW/glfw3.h>

class Sandbox : public VE::Application {
public:
    Sandbox()
        : VE::Application(VE::RendererAPI::API::OpenGL)
    {
        VE_INFO("Sandbox application created");
        VE::MeshLibrary::Init();
        m_Scene = std::make_shared<VE::Scene>();
    }

    ~Sandbox() override {
        VE_INFO("Sandbox application destroyed");
    }

protected:
    void OnUpdate() override {
        m_Scene->OnUpdate();
    }

    void OnRender() override {
        m_Scene->OnRender();
    }

    void OnImGuiRender() override {
        DrawMainMenuBar();
        DrawHierarchyPanel();
        DrawInspectorPanel();
        DrawSceneInfoPanel();

        if (m_ShowDemo)
            ImGui::ShowDemoWindow(&m_ShowDemo);
    }

    void OnRendererReloaded() override {
        // Called twice during a switch:
        //   1st call: old backend still active → release GPU resources
        //   2nd call: new backend ready → recreate GPU resources
        // We detect which phase by checking if MeshLibrary has resources.

        if (VE::MeshLibrary::GetTriangle()) {
            // Phase 1: release
            ClearEntityGPUResources();
            VE::MeshLibrary::Shutdown();
        } else {
            // Phase 2: recreate
            VE::MeshLibrary::Init();
            RestoreEntityGPUResources();
        }
    }

private:
    // ── GPU resource management for API switching ──────────────────────

    void ClearEntityGPUResources() {
        auto view = m_Scene->GetAllEntitiesWith<VE::MeshRendererComponent>();
        for (auto entityID : view) {
            auto& mr = view.get<VE::MeshRendererComponent>(entityID);
            // Remember which mesh type this entity had
            for (int i = 0; i < VE::MeshLibrary::GetMeshCount(); i++) {
                if (VE::MeshLibrary::GetMeshByIndex(i) == mr.Mesh) {
                    m_EntityMeshIndex[static_cast<uint32_t>(entityID)] = i;
                    break;
                }
            }
            mr.Mesh.reset();
            mr.Material.reset();
        }
    }

    void RestoreEntityGPUResources() {
        auto view = m_Scene->GetAllEntitiesWith<VE::MeshRendererComponent>();
        for (auto entityID : view) {
            auto& mr = view.get<VE::MeshRendererComponent>(entityID);
            auto it = m_EntityMeshIndex.find(static_cast<uint32_t>(entityID));
            if (it != m_EntityMeshIndex.end()) {
                mr.Mesh = VE::MeshLibrary::GetMeshByIndex(it->second);
                mr.Material = VE::MeshLibrary::GetDefaultShader();
            }
        }
        m_EntityMeshIndex.clear();
    }

    // ── Main Menu Bar ──────────────────────────────────────────────────
    void DrawMainMenuBar() {
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Exit"))
                    glfwSetWindowShouldClose(GetWindow().GetNativeWindow(), true);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("GameObject")) {
                if (ImGui::MenuItem("Create Empty"))
                    m_Scene->CreateEntity("GameObject");
                if (ImGui::MenuItem("Create Triangle")) {
                    auto entity = m_Scene->CreateEntity("Triangle");
                    auto& mr = entity.AddComponent<VE::MeshRendererComponent>();
                    mr.Mesh = VE::MeshLibrary::GetTriangle();
                    mr.Material = VE::MeshLibrary::GetDefaultShader();
                }
                if (ImGui::MenuItem("Create Quad")) {
                    auto entity = m_Scene->CreateEntity("Quad");
                    auto& mr = entity.AddComponent<VE::MeshRendererComponent>();
                    mr.Mesh = VE::MeshLibrary::GetQuad();
                    mr.Material = VE::MeshLibrary::GetDefaultShader();
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
                | ImGuiTreeNodeFlags_NoTreePushOnOpen
                | ImGuiTreeNodeFlags_SpanAvailWidth;
            if (isSelected)
                flags |= ImGuiTreeNodeFlags_Selected;

            ImGui::TreeNodeEx(
                reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<uint32_t>(entityID))),
                flags, "%s", tag.Tag.c_str());

            if (ImGui::IsItemClicked()) {
                m_SelectedEntity = VE::Entity(entityID, &*m_Scene);
            }
        }

        if (ImGui::BeginPopupContextWindow("HierarchyPopup", ImGuiPopupFlags_NoOpenOverItems | ImGuiPopupFlags_MouseButtonRight)) {
            if (ImGui::MenuItem("Create Empty"))
                m_Scene->CreateEntity("GameObject");
            if (ImGui::MenuItem("Create Triangle")) {
                auto entity = m_Scene->CreateEntity("Triangle");
                auto& mr = entity.AddComponent<VE::MeshRendererComponent>();
                mr.Mesh = VE::MeshLibrary::GetTriangle();
                mr.Material = VE::MeshLibrary::GetDefaultShader();
            }
            if (ImGui::MenuItem("Create Quad")) {
                auto entity = m_Scene->CreateEntity("Quad");
                auto& mr = entity.AddComponent<VE::MeshRendererComponent>();
                mr.Mesh = VE::MeshLibrary::GetQuad();
                mr.Material = VE::MeshLibrary::GetDefaultShader();
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

        // Tag (name)
        if (m_SelectedEntity.HasComponent<VE::TagComponent>()) {
            auto& tag = m_SelectedEntity.GetComponent<VE::TagComponent>();
            char buffer[256];
            strncpy(buffer, tag.Tag.c_str(), sizeof(buffer) - 1);
            buffer[sizeof(buffer) - 1] = '\0';
            if (ImGui::InputText("Name", buffer, sizeof(buffer))) {
                tag.Tag = buffer;
            }
            ImGui::Separator();
        }

        // Transform
        if (m_SelectedEntity.HasComponent<VE::TransformComponent>()) {
            if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
                auto& tc = m_SelectedEntity.GetComponent<VE::TransformComponent>();
                ImGui::DragFloat3("Position", tc.Position.data(), 0.1f);
                ImGui::DragFloat3("Rotation", tc.Rotation.data(), 1.0f);
                ImGui::DragFloat3("Scale",    tc.Scale.data(),    0.1f, 0.01f, 100.0f);
            }
            ImGui::Separator();
        }

        // MeshRenderer
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
                            mr.Material = VE::MeshLibrary::GetDefaultShader();
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }

                ImGui::ColorEdit4("Color", mr.Color.data());

                if (ImGui::Button("Remove Component"))
                    removeComponent = true;
            }
            if (removeComponent) {
                m_SelectedEntity.RemoveComponent<VE::MeshRendererComponent>();
            }
            ImGui::Separator();
        }

        // Add Component button
        if (ImGui::Button("Add Component", ImVec2(-1, 0)))
            ImGui::OpenPopup("AddComponentPopup");

        if (ImGui::BeginPopup("AddComponentPopup")) {
            if (!m_SelectedEntity.HasComponent<VE::MeshRendererComponent>()) {
                if (ImGui::MenuItem("Mesh Renderer")) {
                    auto& mr = m_SelectedEntity.AddComponent<VE::MeshRendererComponent>();
                    mr.Mesh = VE::MeshLibrary::GetTriangle();
                    mr.Material = VE::MeshLibrary::GetDefaultShader();
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
        ImGui::Text("VibeEngine v0.1.0");
        ImGui::Separator();

        // Renderer backend dropdown
        const char* apiNames[] = { "OpenGL", "Vulkan" };
        int currentAPI = (GetCurrentAPI() == VE::RendererAPI::API::OpenGL) ? 0 : 1;
        if (ImGui::Combo("Renderer", &currentAPI, apiNames, 2)) {
            auto newAPI = (currentAPI == 0)
                ? VE::RendererAPI::API::OpenGL
                : VE::RendererAPI::API::Vulkan;
            RequestSwitchAPI(newAPI);
        }

        // Count renderable entities
        int drawCalls = 0;
        auto view = m_Scene->GetAllEntitiesWith<VE::MeshRendererComponent>();
        for (auto e : view) {
            auto& mr = view.get<VE::MeshRendererComponent>(e);
            if (mr.Mesh && mr.Material) drawCalls++;
        }
        ImGui::Text("Entities: %d", static_cast<int>(m_Scene->GetRegistry().storage<entt::entity>().size()));
        ImGui::Text("Draw calls: %d", drawCalls);
        ImGui::Separator();
        ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
        ImGui::Text("Frame time: %.3f ms", 1000.0f / ImGui::GetIO().Framerate);
        ImGui::End();
    }

private:
    std::shared_ptr<VE::Scene> m_Scene;
    VE::Entity m_SelectedEntity;

    // Temporary storage for mesh indices during API switch
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
