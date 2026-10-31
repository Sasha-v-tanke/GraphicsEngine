#pragma once
#include <GraphicsEngine/graphics/graphics.h>
namespace NGraphics::NBackend {
// Backend integration shares the global owner namespace with Graphics. This
// access boundary is internal; completion identities remain opaque to callers.
class CompletionAccess final {
public:
    [[nodiscard]] static std::uint64_t NewOwner() {
        return Graphics::AcquireOwnerId();
    }
    [[nodiscard]] static CompletionPoint Make(std::uint64_t owner, std::uint64_t value) {
        return {owner, value};
    }
    [[nodiscard]] static bool Owns(CompletionPoint point, std::uint64_t owner) {
        return point.m_ownerId == owner;
    }
};
} // namespace NGraphics::NBackend
