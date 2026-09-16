#pragma once

#include <system_error>

namespace NCommon {

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

std::error_code MakeErrorCode(EError error) noexcept;

} // namespace NCommon
