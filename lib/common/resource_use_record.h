#pragma once

#include <cstdint>
#include <memory>
#include <utility>

namespace NCommon {

class ResourceUseRecord final {
public:
    ResourceUseRecord() = default;

    ResourceUseRecord(std::uint64_t version, std::shared_ptr<const void> payload) noexcept
        : m_version(version)
        , m_payload(std::move(payload)) {
    }

    [[nodiscard]] bool IsValid() const noexcept {
        return m_version != 0 && static_cast<bool>(m_payload);
    }

    [[nodiscard]] std::uint64_t GetVersion() const noexcept {
        return m_version;
    }

private:
    std::uint64_t m_version = 0;
    std::shared_ptr<const void> m_payload;
};

} // namespace NCommon
