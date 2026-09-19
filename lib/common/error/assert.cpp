#include "assert.h"

#include <string>

namespace NCommon {

AssertionFailure::AssertionFailure(std::string_view message, std::source_location location)
    : std::logic_error(std::string{message})
    , m_location(location) {
}

const std::source_location& AssertionFailure::GetLocation() const noexcept {
    return m_location;
}

void Assert(bool condition, std::string_view expression, std::source_location location) {
    if (condition) {
        return;
    }

    throw AssertionFailure{
            std::string("Assertion failed: ").append(expression),
            location,
    };
}

} // namespace NCommon
