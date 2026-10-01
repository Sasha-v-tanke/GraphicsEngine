#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace NResources {

class ResourceIdentity final {
public:
    ResourceIdentity() = default;

    ResourceIdentity(std::string resourceClass, std::string key)
        : m_resourceClass(std::move(resourceClass))
        , m_key(std::move(key)) {
    }

    [[nodiscard]] static ResourceIdentity FromPath(std::string resourceClass, const std::filesystem::path& path) {
        return ResourceIdentity{std::move(resourceClass), path.lexically_normal().generic_string()};
    }

    [[nodiscard]] std::string_view GetResourceClass() const noexcept {
        return m_resourceClass;
    }

    [[nodiscard]] std::string_view GetKey() const noexcept {
        return m_key;
    }

    [[nodiscard]] bool IsValid() const noexcept {
        return !m_resourceClass.empty() && !m_key.empty();
    }

    [[nodiscard]] friend bool operator==(const ResourceIdentity& lhs, const ResourceIdentity& rhs) noexcept = default;

private:
    std::string m_resourceClass;
    std::string m_key;
};

struct ResourceIdentityHash final {
    [[nodiscard]] std::size_t operator()(const ResourceIdentity& identity) const noexcept;
};

} // namespace NResources
