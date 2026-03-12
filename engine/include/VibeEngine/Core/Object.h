/*
 * Object — Base class for all engine objects (assets, entities, resources).
 *
 * Mirrors Unity's UnityEngine.Object concept: every identifiable object has
 * a name and a unique ID.  Asset types (MeshAsset, Material, Texture, etc.)
 * and Entity both derive from Object.
 */
#pragma once

#include "VibeEngine/Core/UUID.h"
#include <string>

namespace VE {

class Object {
public:
    Object() = default;
    Object(const std::string& name) : m_Name(name) {}
    Object(const std::string& name, UUID uuid) : m_Name(name), m_InstanceID(uuid) {}
    virtual ~Object() = default;

    const std::string& GetName() const { return m_Name; }
    void SetName(const std::string& name) { m_Name = name; }

    UUID GetInstanceID() const { return m_InstanceID; }

protected:
    std::string m_Name;
    UUID m_InstanceID;
};

} // namespace VE
