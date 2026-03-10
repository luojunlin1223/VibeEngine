#pragma once

#include <string>
#include <memory>
#include <cstdint>

namespace VE {

class Shader {
public:
    virtual ~Shader() = default;

    virtual void Bind() const = 0;
    virtual void Unbind() const = 0;

    static std::shared_ptr<Shader> Create(const std::string& vertexSrc, const std::string& fragmentSrc);
};

} // namespace VE
