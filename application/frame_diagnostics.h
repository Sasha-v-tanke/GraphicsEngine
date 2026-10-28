#pragma once
#include <cstdint>
namespace NApplication {
struct FrameDiagnostics {
    std::uint64_t Submitted = 0;
    std::uint64_t Completed = 0;
    std::uint64_t Recycled = 0;
    std::uint64_t Abandoned = 0;
};
} // namespace NApplication
