#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace VE {

// ── Shader data types ───────────────────────────────────────────────

enum class ShaderDataType {
    None = 0, Float, Float2, Float3, Float4,
    Int, Int2, Int3, Int4,
    Mat3, Mat4, Bool
};

uint32_t ShaderDataTypeSize(ShaderDataType type);

// ── Buffer layout ───────────────────────────────────────────────────

struct BufferElement {
    std::string    Name;
    ShaderDataType Type;
    uint32_t       Size;
    uint32_t       Offset;
    bool           Normalized;

    BufferElement(ShaderDataType type, const std::string& name, bool normalized = false);

    uint32_t GetComponentCount() const;
};

class BufferLayout {
public:
    BufferLayout() = default;
    BufferLayout(std::initializer_list<BufferElement> elements);

    const std::vector<BufferElement>& GetElements() const { return m_Elements; }
    uint32_t GetStride() const { return m_Stride; }

    auto begin() const { return m_Elements.begin(); }
    auto end()   const { return m_Elements.end(); }

private:
    void CalculateOffsetsAndStride();

    std::vector<BufferElement> m_Elements;
    uint32_t m_Stride = 0;
};

// ── Vertex buffer ───────────────────────────────────────────────────

class VertexBuffer {
public:
    virtual ~VertexBuffer() = default;

    virtual void Bind() const = 0;
    virtual void Unbind() const = 0;
    virtual void SetData(const void* data, uint32_t size) = 0;
    virtual void SetLayout(const BufferLayout& layout) = 0;
    virtual const BufferLayout& GetLayout() const = 0;

    static std::shared_ptr<VertexBuffer> Create(const float* vertices, uint32_t size);
};

// ── Index buffer ────────────────────────────────────────────────────

class IndexBuffer {
public:
    virtual ~IndexBuffer() = default;

    virtual void Bind() const = 0;
    virtual void Unbind() const = 0;
    virtual uint32_t GetCount() const = 0;

    static std::shared_ptr<IndexBuffer> Create(const uint32_t* indices, uint32_t count);
};

} // namespace VE
