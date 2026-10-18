#pragma once

#include <cstdint>
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

struct SystemSchedule {
    std::uint64_t Id = 0;
    std::vector<std::uint64_t> Before = {};
    std::vector<std::uint64_t> After = {};
};

} // namespace NEcs
