#include "VibeEngine/Scene/Scene.h"
#include "VibeEngine/Scene/Entity.h"
#include "VibeEngine/Scene/Components.h"
#include "VibeEngine/Renderer/RenderCommand.h"
#include "VibeEngine/Core/Log.h"

#include <sstream>

namespace VE {

Entity Scene::CreateEntity(const std::string& name) {
    Entity entity(m_Registry.create(), this);

    // Generate a unique name if the default is used
    std::string entityName = name;
    if (name == "GameObject") {
        std::ostringstream oss;
        oss << "GameObject_" << m_EntityCounter;
        entityName = oss.str();
    }
    m_EntityCounter++;

    entity.AddComponent<TagComponent>(entityName);
    entity.AddComponent<TransformComponent>();

    VE_ENGINE_INFO("Entity created: {0}", entityName);
    return entity;
}

void Scene::DestroyEntity(Entity entity) {
    if (entity.HasComponent<TagComponent>()) {
        VE_ENGINE_INFO("Entity destroyed: {0}", entity.GetComponent<TagComponent>().Tag);
    }
    m_Registry.destroy(entity.GetHandle());
}

void Scene::OnUpdate() {
    // Future: physics, scripts, etc.
}

void Scene::OnRender() {
    auto view = m_Registry.view<MeshRendererComponent>();
    for (auto entityID : view) {
        auto& meshRenderer = view.get<MeshRendererComponent>(entityID);

        if (!meshRenderer.Mesh || !meshRenderer.Material)
            continue;

        meshRenderer.Material->Bind();
        RenderCommand::DrawIndexed(meshRenderer.Mesh);
    }
}

} // namespace VE
