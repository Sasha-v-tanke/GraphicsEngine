#pragma once

#include <cstddef>
#include <cstdint>

#include <GraphicsEngine/graphics/graphics_pipeline.h>

namespace NGraphics {

// Immutable, owning descriptor snapshot. Equality compares the complete description,
// not just its hash. Shader snapshots remain valid across Resources unload/reload.
// The current layout is empty and viewport/scissor are dynamic for every pipeline;
// future layout/dynamic-state policies must become explicit descriptor/key fields.
class PipelineKey final {
public:
    explicit PipelineKey(GraphicsPipelineDescriptor descriptor);

    [[nodiscard]] const GraphicsPipelineDescriptor& GetDescriptor() const noexcept;
    [[nodiscard]] std::uint64_t GetHash() const noexcept;

    friend bool operator==(const PipelineKey& left, const PipelineKey& right) noexcept;

private:
    GraphicsPipelineDescriptor m_descriptor;
    std::uint64_t m_hash = 0;
};

struct PipelineKeyHash {
    [[nodiscard]] std::size_t operator()(const PipelineKey& key) const noexcept;
};

} // namespace NGraphics
