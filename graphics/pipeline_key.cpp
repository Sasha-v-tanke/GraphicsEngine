#include "pipeline_key.h"

#include <utility>

namespace NGraphics {

PipelineKey::PipelineKey(GraphicsPipelineDescriptor descriptor)
    : m_descriptor(std::move(descriptor)) {
    ValidateGraphicsPipelineDescriptor(m_descriptor);
    m_hash = HashGraphicsPipelineDescriptor(m_descriptor);
}

const GraphicsPipelineDescriptor& PipelineKey::GetDescriptor() const noexcept {
    return m_descriptor;
}

std::uint64_t PipelineKey::GetHash() const noexcept {
    return m_hash;
}

bool operator==(const PipelineKey& left, const PipelineKey& right) noexcept {
    return left.m_hash == right.m_hash && left.m_descriptor == right.m_descriptor;
}

std::size_t PipelineKeyHash::operator()(const PipelineKey& key) const noexcept {
    return static_cast<std::size_t>(key.GetHash());
}

} // namespace NGraphics
