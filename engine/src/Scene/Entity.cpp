#include "VibeEngine/Scene/Entity.h"
#include "VibeEngine/Scene/Components.h"

namespace VE {

Entity Entity::GetParent() const {
    if (!m_Scene || m_Handle == entt::null) return {};
    auto& registry = m_Scene->GetRegistry();
    if (!registry.all_of<RelationshipComponent>(m_Handle)) return {};
    auto& rel = registry.get<RelationshipComponent>(m_Handle);
    if (rel.Parent == entt::null || !registry.valid(rel.Parent)) return {};
    return Entity(rel.Parent, m_Scene);
}

std::vector<Entity> Entity::GetChildren() const {
    std::vector<Entity> result;
    if (!m_Scene || m_Handle == entt::null) return result;
    auto& registry = m_Scene->GetRegistry();
    if (!registry.all_of<RelationshipComponent>(m_Handle)) return result;
    auto& rel = registry.get<RelationshipComponent>(m_Handle);
    result.reserve(rel.Children.size());
    for (auto child : rel.Children) {
        if (registry.valid(child))
            result.emplace_back(child, m_Scene);
    }
    return result;
}

} // namespace VE
