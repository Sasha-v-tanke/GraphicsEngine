#include "error.h"

#include <string>
#include <system_error>

namespace NCommon {

namespace {

class ErrorCategory final: public std::error_category {
public:
    [[nodiscard]] const char* name() const noexcept override {
        return "graphics_engine.common";
    }

    [[nodiscard]] std::string message(int value) const override {
        switch (static_cast<EError>(value)) {
        case EError::UNKNOWN:
            return "Unknown error";
        case EError::INVALID_ARGUMENT:
            return "Invalid argument";
        case EError::INVALID_STATE:
            return "Invalid state";
        case EError::NOT_FOUND:
            return "Not found";
        case EError::ALREADY_EXISTS:
            return "Already exists";
        case EError::OUT_OF_MEMORY:
            return "Out of memory";
        case EError::IO_ERROR:
            return "I/O error";
        case EError::UNSUPPORTED:
            return "Unsupported operation";
        case EError::NOT_IMPLEMENTED:
            return "Not implemented";
        }

        return "Unrecognized common error";
    }
};

const std::error_category& GetErrorCategory() noexcept {
    static const ErrorCategory category;
    return category;
}

} // namespace

std::error_code make_error_code(EError error) noexcept {
    return {
            static_cast<int>(error),
            GetErrorCategory(),
    };
}

} // namespace NCommon
