/*
 * UUID — Simple 64-bit unique identifier for entities.
 *
 * Uses std::mt19937_64 seeded with std::random_device for generation.
 * Provides hash specialization for use as map/set keys.
 */
#pragma once

#include <cstdint>
#include <random>
#include <functional>

namespace VE {

class UUID {
public:
    UUID() : m_UUID(Generate()) {}
    UUID(uint64_t uuid) : m_UUID(uuid) {}

    operator uint64_t() const { return m_UUID; }

    bool operator==(const UUID& other) const { return m_UUID == other.m_UUID; }
    bool operator!=(const UUID& other) const { return m_UUID != other.m_UUID; }

private:
    static uint64_t Generate() {
        static std::random_device rd;
        static std::mt19937_64 engine(rd());
        static std::uniform_int_distribution<uint64_t> dist;
        return dist(engine);
    }

    uint64_t m_UUID;
};

} // namespace VE

namespace std {
template<>
struct hash<VE::UUID> {
    size_t operator()(const VE::UUID& uuid) const {
        return hash<uint64_t>()(static_cast<uint64_t>(uuid));
    }
};
} // namespace std
