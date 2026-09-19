#pragma once

#include <source_location>
#include <stdexcept>
#include <string_view>

namespace NCommon {

class AssertionFailure final: public std::logic_error {
public:
    AssertionFailure(std::string_view message, std::source_location location);

    [[nodiscard]] const std::source_location& GetLocation() const noexcept;

private:
    std::source_location m_location;
};

void Assert(bool condition,
            std::string_view expression,
            std::source_location location = std::source_location::current());

} // namespace NCommon

#define GRAPHICS_ENGINE_ASSERT(expression)                                                                             \
    ::NCommon::Assert(static_cast<bool>(expression), #expression, std::source_location::current())
