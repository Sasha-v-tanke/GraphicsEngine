#pragma once

#include <cstdint>
#include <memory>
#include <utility>

#include <GraphicsEngine/resources/resource_handle.h>
#include <GraphicsEngine/resources/resource_use_record.h>

namespace NResources {

class ResourceManager;

template<typename T>
class ResourceLease final {
public:
    ResourceLease() = default;

    ResourceLease(const ResourceLease&) = delete;
    ResourceLease& operator=(const ResourceLease&) = delete;

    ResourceLease(ResourceLease&&) noexcept = default;
    ResourceLease& operator=(ResourceLease&&) noexcept = default;

    [[nodiscard]] bool IsValid() const noexcept {
        return m_resource.IsValid() && m_version != 0 && static_cast<bool>(m_payload);
    }

    [[nodiscard]] ResourceHandle<T> GetResource() const noexcept {
        return m_resource;
    }

    [[nodiscard]] std::uint64_t GetVersion() const noexcept {
        return m_version;
    }

    [[nodiscard]] const T* Get() const noexcept {
        return m_payload.get();
    }

    [[nodiscard]] const T& operator*() const noexcept {
        return *m_payload;
    }

    [[nodiscard]] const T* operator->() const noexcept {
        return m_payload.get();
    }

    [[nodiscard]] ResourceUseRecord GetUseRecord() const noexcept {
        return ResourceUseRecord{
                m_version,
                m_payload,
        };
    }

private:
    ResourceLease(ResourceHandle<T> resource, std::uint64_t version, std::shared_ptr<const T> payload) noexcept
        : m_resource(resource)
        , m_version(version)
        , m_payload(std::move(payload)) {
    }

private:
    ResourceHandle<T> m_resource;
    std::uint64_t m_version = 0;
    std::shared_ptr<const T> m_payload;

    friend class ResourceManager;
};

} // namespace NResources
