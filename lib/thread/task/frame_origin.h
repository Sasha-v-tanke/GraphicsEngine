#pragma once

#include <cstdint>
#include <optional>
#include <utility>

namespace NCommon {

struct FrameOrigin {
    std::uint64_t RunGeneration = 0;
    std::uint64_t FrameSequence = 0;
    friend bool operator==(const FrameOrigin&, const FrameOrigin&) = default;
};

// Invocation-local metadata, never a World access grant or a frame lifetime lease.
class FrameOriginScope final {
public:
    explicit FrameOriginScope(std::optional<FrameOrigin> origin) noexcept
        : m_previous(std::exchange(s_current, origin)) {
    }
    ~FrameOriginScope() {
        s_current = m_previous;
    }
    FrameOriginScope(const FrameOriginScope&) = delete;
    FrameOriginScope& operator=(const FrameOriginScope&) = delete;
    static std::optional<FrameOrigin> Current() noexcept {
        return s_current;
    }

private:
    inline static thread_local std::optional<FrameOrigin> s_current;
    std::optional<FrameOrigin> m_previous;
};

template<typename Function>
Function BindFrameOrigin(Function function, std::optional<FrameOrigin> origin) {
    if (!origin || !function) {
        return function;
    }
    return [function = std::move(function), origin](auto& context) mutable {
        const FrameOriginScope scope{origin};
        function(context);
    };
}

} // namespace NCommon
