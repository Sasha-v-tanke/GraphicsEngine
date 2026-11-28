#pragma once

#include <cstdint>
#include <type_traits>
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

namespace NSystem {

template<typename T>
struct Read {
    static_assert(std::is_same_v<T, std::remove_cvref_t<T>> && !std::is_void_v<T>);
    using Component = T;
    static constexpr ESystemAccessMode MODE = ESystemAccessMode::READ;
};

template<typename T>
struct Write {
    static_assert(std::is_same_v<T, std::remove_cvref_t<T>> && !std::is_void_v<T>);
    using Component = T;
    static constexpr ESystemAccessMode MODE = ESystemAccessMode::WRITE;
};

template<typename... TDeclarations>
struct Access {
    static_assert(((std::is_same_v<TDeclarations, Read<typename TDeclarations::Component>> ||
                    std::is_same_v<TDeclarations, Write<typename TDeclarations::Component>>) &&
                   ...));

    template<typename T>
    static constexpr bool CAN_READ = (std::is_same_v<std::remove_const_t<T>, typename TDeclarations::Component> || ...);

    template<typename T>
    static constexpr bool CAN_WRITE = ((std::is_same_v<T, typename TDeclarations::Component> &&
                                        TDeclarations::MODE == ESystemAccessMode::WRITE) ||
                                       ...);

    template<typename T>
    using Component = std::conditional_t<CAN_WRITE<T>, T, const std::remove_const_t<T>>;
};

} // namespace NSystem

} // namespace NEcs
