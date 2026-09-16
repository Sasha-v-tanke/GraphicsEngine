#pragma once

#include <format>
#include <source_location>
#include <string>
#include <system_error>
#include <utility>

namespace NCommon {

class Exception final: public std::system_error {
public:
    Exception(std::error_code code, std::string message, std::source_location location)
        : std::system_error(code, message)
        , m_message(std::move(message))
        , m_location(location) {
    }

    [[nodiscard]] const std::string& GetMessage() const noexcept {
        return m_message;
    }

    [[nodiscard]] const std::source_location& GetLocation() const noexcept {
        return m_location;
    }

private:
    std::string m_message;
    std::source_location m_location;
};

template<typename... Args>
[[noreturn]] void
Throw(std::error_code code, std::source_location location, std::format_string<Args...> format, Args&&... args) {
    throw Exception{
            code,
            std::format(format, std::forward<Args>(args)...),
            location,
    };
}

} // namespace NCommon

#define GRAPHICS_ENGINE_THROW(error, format, ...)                                                                      \
    ::NCommon::Throw(error, std::source_location::current(), format __VA_OPT__(, ) __VA_ARGS__)
