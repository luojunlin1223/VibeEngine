#pragma once

#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <cstdint>

namespace VE {

class ComputeShader {
public:
    virtual ~ComputeShader() = default;

    virtual void Bind() const = 0;
    virtual void Unbind() const = 0;
    virtual void Dispatch(uint32_t groupX, uint32_t groupY, uint32_t groupZ) const = 0;
    virtual void MemoryBarrier(uint32_t barrierBits) const = 0;

    virtual void SetMat4(const std::string& name, const glm::mat4& value) = 0;
    virtual void SetVec3(const std::string& name, const glm::vec3& value) = 0;
    virtual void SetFloat(const std::string& name, float value) = 0;
    virtual void SetInt(const std::string& name, int value) = 0;

    const std::string& GetName() const { return m_Name; }
    void SetName(const std::string& name) { m_Name = name; }

    static std::shared_ptr<ComputeShader> Create(const std::string& computeSrc);
    static std::shared_ptr<ComputeShader> CreateFromFile(const std::string& filePath);

protected:
    std::string m_Name;
};

} // namespace VE
