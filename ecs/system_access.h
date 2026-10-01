#pragma once

#include <typeindex>
#include <vector>

namespace NEcs {

enum class ESystemAccessMode {
    READ,
    WRITE,
};

struct SystemAccess {
    std::type_index Component = typeid(void);
    ESystemAccessMode Mode = ESystemAccessMode::READ;
};

template<typename T>
[[nodiscard]] SystemAccess Read() noexcept {
    return {
            .Component = typeid(T),
            .Mode = ESystemAccessMode::READ,
    };
}

template<typename T>
[[nodiscard]] SystemAccess Write() noexcept {
    return {
            .Component = typeid(T),
            .Mode = ESystemAccessMode::WRITE,
    };
}

using SystemAccessList = std::vector<SystemAccess>;

} // namespace NEcs
