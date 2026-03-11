#include "VibeEngine/Scene/Scene.h"
#include "VibeEngine/Scene/Entity.h"
#include "VibeEngine/Scene/Components.h"
#include "VibeEngine/Scene/MeshLibrary.h"
#include "VibeEngine/Renderer/RenderCommand.h"
#include "VibeEngine/Core/Log.h"

#include <glm/gtc/matrix_transform.hpp>
#include <sstream>

namespace VE {

Entity Scene::CreateEntity(const std::string& name) {
    return CreateEntityWithUUID(UUID(), name);
}

Entity Scene::CreateEntityWithUUID(UUID uuid, const std::string& name) {
    Entity entity(m_Registry.create(), this);

    // Generate a unique name if the default is used
    std::string entityName = name;
    if (name == "GameObject") {
        std::ostringstream oss;
        oss << "GameObject_" << m_EntityCounter;
        entityName = oss.str();
    }
    m_EntityCounter++;

    entity.AddComponent<IDComponent>(uuid);
    entity.AddComponent<TagComponent>(entityName);
    entity.AddComponent<TransformComponent>();
    entity.AddComponent<RelationshipComponent>();

    VE_ENGINE_INFO("Entity created: {0}", entityName);
    return entity;
}

void Scene::DestroyEntity(Entity entity) {
    if (entity.HasComponent<TagComponent>()) {
        VE_ENGINE_INFO("Entity destroyed: {0}", entity.GetComponent<TagComponent>().Tag);
    }

    // Remove from parent's children list
    RemoveParent(entity.GetHandle());

    // Recursively destroy children
    if (entity.HasComponent<RelationshipComponent>()) {
        auto children = entity.GetComponent<RelationshipComponent>().Children; // copy
        for (auto child : children) {
            if (m_Registry.valid(child))
                DestroyEntity(Entity(child, this));
        }
    }

    // Clean up Jolt body if physics is running
    if (m_PhysicsWorld && entity.HasComponent<RigidbodyComponent>()) {
        auto& rb = entity.GetComponent<RigidbodyComponent>();
        m_PhysicsWorld->RemoveBody(rb._JoltBodyID);
    }
    m_Registry.destroy(entity.GetHandle());
}

void Scene::SetParent(entt::entity child, entt::entity parent) {
    if (child == parent) return;
    if (!m_Registry.valid(child) || !m_Registry.valid(parent)) return;

    // Prevent circular: walk up from parent, if we hit child, it's circular
    entt::entity check = parent;
    while (check != entt::null) {
        if (check == child) return; // circular!
        auto& rel = m_Registry.get<RelationshipComponent>(check);
        check = rel.Parent;
    }

    RemoveParent(child);

    auto& childRel = m_Registry.get<RelationshipComponent>(child);
    auto& parentRel = m_Registry.get<RelationshipComponent>(parent);
    childRel.Parent = parent;
    parentRel.Children.push_back(child);
}

void Scene::RemoveParent(entt::entity child) {
    if (!m_Registry.valid(child)) return;
    auto& childRel = m_Registry.get<RelationshipComponent>(child);
    if (childRel.Parent == entt::null) return;

    if (m_Registry.valid(childRel.Parent)) {
        auto& parentRel = m_Registry.get<RelationshipComponent>(childRel.Parent);
        auto& vec = parentRel.Children;
        vec.erase(std::remove(vec.begin(), vec.end(), child), vec.end());
    }
    childRel.Parent = entt::null;
}

glm::mat4 Scene::GetWorldTransform(entt::entity entity) const {
    if (!m_Registry.valid(entity) || !m_Registry.all_of<TransformComponent>(entity))
        return glm::mat4(1.0f);

    auto& tc = m_Registry.get<TransformComponent>(entity);
    glm::mat4 local = glm::translate(glm::mat4(1.0f),
        glm::vec3(tc.Position[0], tc.Position[1], tc.Position[2]));
    local = glm::rotate(local, glm::radians(tc.Rotation[0]), glm::vec3(1, 0, 0));
    local = glm::rotate(local, glm::radians(tc.Rotation[1]), glm::vec3(0, 1, 0));
    local = glm::rotate(local, glm::radians(tc.Rotation[2]), glm::vec3(0, 0, 1));
    local = glm::scale(local, glm::vec3(tc.Scale[0], tc.Scale[1], tc.Scale[2]));

    if (m_Registry.all_of<RelationshipComponent>(entity)) {
        auto& rel = m_Registry.get<RelationshipComponent>(entity);
        if (rel.Parent != entt::null && m_Registry.valid(rel.Parent))
            return GetWorldTransform(rel.Parent) * local;
    }

    return local;
}

void Scene::OnUpdate(float deltaTime) {
    if (m_PhysicsRunning && m_PhysicsWorld) {
        static constexpr float PHYSICS_DT = 1.0f / 60.0f;
        m_PhysicsAccumulator += deltaTime;
        while (m_PhysicsAccumulator >= PHYSICS_DT) {
            m_PhysicsWorld->Step(PHYSICS_DT);
            m_PhysicsAccumulator -= PHYSICS_DT;
        }
        m_PhysicsWorld->SyncTransformsToScene(m_Registry);
    }
}

void Scene::StartPhysics() {
    if (m_PhysicsRunning) return;
    m_PhysicsWorld = std::make_unique<PhysicsWorld>();
    m_PhysicsWorld->SyncBodiesFromScene(m_Registry);
    m_PhysicsAccumulator = 0.0f;
    m_PhysicsRunning = true;
    VE_ENGINE_INFO("Physics simulation started");
}

void Scene::StopPhysics() {
    if (!m_PhysicsRunning) return;
    // Destroy all Jolt bodies
    auto view = m_Registry.view<RigidbodyComponent>();
    for (auto entity : view) {
        auto& rb = view.get<RigidbodyComponent>(entity);
        if (rb._JoltBodyID != 0xFFFFFFFF && m_PhysicsWorld) {
            m_PhysicsWorld->RemoveBody(rb._JoltBodyID);
            rb._JoltBodyID = 0xFFFFFFFF;
        }
    }
    m_PhysicsWorld.reset();
    m_PhysicsRunning = false;
    VE_ENGINE_INFO("Physics simulation stopped");
}

static glm::mat4 ComputeModelMatrix(const TransformComponent& tc) {
    glm::mat4 model = glm::translate(glm::mat4(1.0f),
        glm::vec3(tc.Position[0], tc.Position[1], tc.Position[2]));
    model = glm::rotate(model, glm::radians(tc.Rotation[0]), glm::vec3(1, 0, 0));
    model = glm::rotate(model, glm::radians(tc.Rotation[1]), glm::vec3(0, 1, 0));
    model = glm::rotate(model, glm::radians(tc.Rotation[2]), glm::vec3(0, 0, 1));
    model = glm::scale(model, glm::vec3(tc.Scale[0], tc.Scale[1], tc.Scale[2]));
    return model;
}

void Scene::OnRenderSky(const glm::mat4& skyViewProjection) {
    if (!m_PipelineSettings.SkyEnabled) return;

    auto skyShader = MeshLibrary::GetSkyShader();
    auto skyMesh   = MeshLibrary::GetSphere();
    if (!skyShader || !skyMesh) return;

    auto& sky = m_PipelineSettings;

    RenderCommand::SetDepthWrite(false);
    RenderCommand::SetDepthFunc(RendererAPI::DepthFunc::LessEqual);

    skyShader->Bind();
    skyShader->SetMat4("u_MVP", skyViewProjection);
    skyShader->SetVec3("u_TopColor",
        glm::vec3(sky.SkyTopColor[0], sky.SkyTopColor[1], sky.SkyTopColor[2]));
    skyShader->SetVec3("u_BottomColor",
        glm::vec3(sky.SkyBottomColor[0], sky.SkyBottomColor[1], sky.SkyBottomColor[2]));

    if (sky.SkyTexture) {
        sky.SkyTexture->Bind(0);
        skyShader->SetInt("u_Texture", 0);
        skyShader->SetInt("u_UseTexture", 1);
    } else {
        skyShader->SetInt("u_UseTexture", 0);
    }

    RenderCommand::DrawIndexed(skyMesh);

    RenderCommand::SetDepthWrite(true);
    RenderCommand::SetDepthFunc(RendererAPI::DepthFunc::Less);
}

void Scene::OnRender(const glm::mat4& viewProjection, const glm::vec3& cameraPos) {
    // Find directional light in the scene (use first one found, fallback to default)
    glm::vec3 lightDir = glm::normalize(glm::vec3(0.3f, 1.0f, 0.5f));
    glm::vec3 lightColor(1.0f);
    float lightIntensity = 1.0f;
    {
        auto lightView = m_Registry.view<DirectionalLightComponent>();
        for (auto lightEntity : lightView) {
            auto& dl = lightView.get<DirectionalLightComponent>(lightEntity);
            glm::vec3 dir(dl.Direction[0], dl.Direction[1], dl.Direction[2]);
            float len = glm::length(dir);
            if (len > 0.0001f)
                lightDir = dir / len;
            lightColor = glm::vec3(dl.Color[0], dl.Color[1], dl.Color[2]);
            lightIntensity = dl.Intensity;
            break; // use first directional light
        }
    }

    auto view = m_Registry.view<TransformComponent, MeshRendererComponent>();
    for (auto entityID : view) {
        auto [tc, mr] = view.get<TransformComponent, MeshRendererComponent>(entityID);

        if (!mr.Mesh || !mr.Mat)
            continue;

        glm::mat4 model = GetWorldTransform(entityID);
        glm::mat4 mvp = viewProjection * model;

        // Bind material (shader + material properties including textures)
        mr.Mat->Bind();

        auto shader = mr.Mat->GetShader();
        if (!shader) continue;

        shader->SetMat4("u_MVP", mvp);

        // If material has no texture property set, ensure UseTexture=0
        bool hasTexInMat = false;
        for (auto& prop : mr.Mat->GetProperties()) {
            if (prop.Type == MaterialPropertyType::Texture2D && prop.TextureRef)
                hasTexInMat = true;
        }
        if (!hasTexInMat)
            shader->SetInt("u_UseTexture", 0);

        // Lighting + PBR uniforms for lit materials
        if (mr.Mat->IsLit()) {
            shader->SetMat4("u_Model", model);
            shader->SetVec3("u_LightDir", lightDir);
            shader->SetVec3("u_LightColor", lightColor);
            shader->SetFloat("u_LightIntensity", lightIntensity);
            shader->SetVec3("u_ViewPos", cameraPos);

            // PBR defaults (if not set by material)
            bool hasMetallic = false, hasRoughness = false, hasAO = false;
            for (auto& prop : mr.Mat->GetProperties()) {
                if (prop.Name == "u_Metallic") hasMetallic = true;
                if (prop.Name == "u_Roughness") hasRoughness = true;
                if (prop.Name == "u_AO") hasAO = true;
            }
            if (!hasMetallic)  shader->SetFloat("u_Metallic", 0.0f);
            if (!hasRoughness) shader->SetFloat("u_Roughness", 0.5f);
            if (!hasAO)        shader->SetFloat("u_AO", 1.0f);
        }

        // Per-instance color override
        shader->SetVec4("u_EntityColor",
            glm::vec4(mr.Color[0], mr.Color[1], mr.Color[2], mr.Color[3]));

        RenderCommand::DrawIndexed(mr.Mesh);
    }
}

} // namespace VE
