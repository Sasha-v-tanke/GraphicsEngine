#pragma once

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace NResources {

struct MeshVertex {
    std::array<float, 3> Position{};
    std::array<float, 3> Normal{};
    std::array<float, 2> Texcoord{};
};

// CPU triangle-list payload. Attribute provenance is separate from the vertex stream.
class MeshData final {
public:
    static constexpr std::uint8_t NORMAL_PRESENT = 1;
    static constexpr std::uint8_t TEXCOORD_PRESENT = 2;

    MeshData() = default;

    MeshData(std::vector<MeshVertex> vertices,
             std::vector<std::uint32_t> indices,
             std::vector<std::uint8_t> attributeMasks)
        : m_vertices(std::move(vertices))
        , m_indices(std::move(indices))
        , m_attributeMasks(std::move(attributeMasks)) {
    }

    [[nodiscard]] const std::vector<MeshVertex>& GetVertices() const noexcept {
        return m_vertices;
    }

    [[nodiscard]] const std::vector<std::uint32_t>& GetIndices() const noexcept {
        return m_indices;
    }

    // One mask per vertex. Missing attributes have zero values and a cleared presence bit.
    [[nodiscard]] const std::vector<std::uint8_t>& GetAttributeMasks() const noexcept {
        return m_attributeMasks;
    }

private:
    std::vector<MeshVertex> m_vertices;
    std::vector<std::uint32_t> m_indices;
    std::vector<std::uint8_t> m_attributeMasks;
};

} // namespace NResources
