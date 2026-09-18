#pragma once

namespace NCommon {

class NonCopyable {
protected:
    NonCopyable() = default;
    NonCopyable(NonCopyable&&) noexcept = default;

    NonCopyable& operator=(NonCopyable&&) noexcept = default;

    ~NonCopyable() = default;

public:
    NonCopyable(const NonCopyable&) = delete;

    NonCopyable& operator=(const NonCopyable&) = delete;
};

} // namespace NCommon
