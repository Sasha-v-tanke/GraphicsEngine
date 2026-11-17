#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <random>
#include <vector>

#include <ecs/world.h>

namespace NEcsAcceptance {

enum class EShape {
    BALANCED,
    STAR,
    BAMBOO,
    RANDOM,
    SKEWED,
};

inline constexpr std::array SHAPES{EShape::BALANCED, EShape::STAR, EShape::BAMBOO, EShape::RANDOM, EShape::SKEWED};
inline constexpr std::array WORKERS{1U, 2U, 4U, 8U, 16U};
inline constexpr std::size_t NO_PARENT = std::numeric_limits<std::size_t>::max();
using Matrix = std::array<float, 16>;

inline Matrix Identity() {
    Matrix value{};
    for (std::size_t index = 0; index < 4; ++index) {
        value[index * 4 + index] = 1;
    }
    return value;
}

// Deliberately independent of NMath multiplication and the runtime topology/cache.
inline Matrix Multiply(const Matrix& left, const Matrix& right) {
    Matrix result{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            for (std::size_t k = 0; k < 4; ++k) {
                result[row * 4 + column] += left[row * 4 + k] * right[k * 4 + column];
            }
        }
    }
    return result;
}

inline NMath::Mat4 Native(const Matrix& value) {
    NMath::Mat4 result;
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            result(row, column) = value[row * 4 + column];
        }
    }
    return result;
}

inline std::uint64_t Work(std::uint64_t value, std::size_t iterations) {
    for (std::size_t index = 0; index < iterations; ++index) {
        value ^= value >> 12;
        value ^= value << 25;
        value ^= value >> 27;
        value *= 2685821657736338717ULL;
    }
    return value;
}

class Forest {
public:
    Forest(NEcs::World& world, std::size_t count, EShape shape)
        : Owner(world)
        , Parents(count, NO_PARENT)
        , Children(count)
        , Locals(count, Identity()) {
        Entities.reserve(count);
        // NOLINTNEXTLINE(cert-msc51-cpp): Replay topology is intentionally reproducible.
        std::mt19937 random{1145};
        for (std::size_t index = 0; index < count; ++index) {
            Entities.push_back(Owner.GetEntityReference(Owner.CreateEntity()));
            if (index == 0) {
                continue;
            }
            std::size_t parent = 0;
            switch (shape) {
            case EShape::BALANCED:
                parent = (index - 1) / 4;
                break;
            case EShape::STAR:
                break;
            case EShape::BAMBOO:
                parent = index - 1;
                break;
            case EShape::RANDOM:
                parent = random() % index;
                break;
            case EShape::SKEWED:
                parent = index % 7 == 0 ? 0 : index - 1;
                break;
            }
            Parents[index] = parent;
        }
        // Descendant-first attachment avoids quadratic ancestor walks in deep fixtures.
        for (std::size_t index = count; index > 1; --index) {
            const std::size_t child = index - 1;
            Children[Parents[child]].push_back(child);
            Owner.DeferReparent(Entities[child], Entities[Parents[child]]);
        }
        Owner.ApplyDeferredStructuralChanges();
    }

    void SetLocal(std::size_t index, const Matrix& value) {
        Owner.SetLocalTransform(Entities[index], Native(value));
        Locals[index] = value;
    }

    void Reparent(std::size_t child, std::size_t parent) {
        Owner.DeferReparent(Entities[child], Entities[parent]);
        Owner.ApplyDeferredStructuralChanges();
        if (Parents[child] == parent) {
            return;
        }
        if (Parents[child] != NO_PARENT) {
            std::erase(Children[Parents[child]], child);
        }
        Parents[child] = parent;
        Children[parent].push_back(child);
    }

    [[nodiscard]] std::vector<std::size_t> Order() const {
        std::vector<std::size_t> result;
        result.reserve(Entities.size());
        std::vector<std::size_t> pending;
        for (std::size_t index = Parents.size(); index > 0; --index) {
            if (Parents[index - 1] == NO_PARENT) {
                pending.push_back(index - 1);
            }
        }
        while (!pending.empty()) {
            const std::size_t index = pending.back();
            pending.pop_back();
            result.push_back(index);
            pending.insert(pending.end(), Children[index].rbegin(), Children[index].rend());
        }
        return result;
    }

    [[nodiscard]] std::vector<Matrix> ResolveReference() const {
        std::vector<Matrix> result(Entities.size());
        for (const std::size_t index: Order()) {
            result[index] =
                    Parents[index] == NO_PARENT ? Locals[index] : Multiply(result[Parents[index]], Locals[index]);
        }
        return result;
    }

    // Own adjacency traversal, not AdaptiveBatchPlanner::RunSerial.
    [[nodiscard]] std::vector<std::uint64_t>
    TraverseReference(bool bottomUp, bool independent, std::size_t cost) const {
        std::vector<std::uint64_t> result(Entities.size());
        auto order = Order();
        if (bottomUp) {
            std::ranges::reverse(order);
        }
        for (const std::size_t index: order) {
            std::uint64_t value = 17;
            if (!bottomUp && !independent && Parents[index] != NO_PARENT) {
                value = result[Parents[index]];
            }
            value = value * 31 + Entities[index].Identity.Index;
            if (bottomUp) {
                for (const std::size_t child: Children[index]) {
                    value = value * 31 + result[child];
                }
            }
            result[index] = Work(value, cost);
        }
        return result;
    }

    NEcs::World& Owner;
    std::vector<NEcs::EntityReference> Entities;
    std::vector<std::size_t> Parents;
    std::vector<std::vector<std::size_t>> Children;
    std::vector<Matrix> Locals;
};

} // namespace NEcsAcceptance
