#pragma once

#include <string>
#include <system_error>
#include <type_traits>

namespace NCommon {

// Engine-wide error category values used by GraphicsEngine exceptions and status reports.
enum class EError {
    UNKNOWN = 1,
    INVALID_ARGUMENT,
    INVALID_STATE,
    NOT_FOUND,
    ALREADY_EXISTS,
    OUT_OF_MEMORY,
    IO_ERROR,
    UNSUPPORTED,
    NOT_IMPLEMENTED,
};

std::error_code make_error_code(EError error) noexcept;

// Stable error payload for asynchronous APIs that cannot rethrow at the observation point.
struct ErrorInfo {
    std::error_code Code;
    std::string Message;
};

} // namespace NCommon

template<>
struct std::is_error_code_enum<NCommon::EError>: true_type {};
