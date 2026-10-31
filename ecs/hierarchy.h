#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>

#include <GraphicsEngine/ecs/entity.h>
#include <GraphicsEngine/math/matrix.h>

namespace NEcs {

struct EntityReference {
    std::uint64_t WorldId = 0;
    Entity Identity;
    [[nodiscard]] friend constexpr bool operator==(EntityReference, EntityReference) noexcept = default;
};

enum class EReparentMode {
    KEEP_LOCAL,
    KEEP_WORLD,
};

namespace NInternal {
struct TopologyState;
struct TopologyLease;
} // namespace NInternal

class World;

// Pins topology and local matrices. Copies share one lease; release all copies before structural commit.
// Retains its own storage after World destruction, but grants no component/World access.
class TopologySnapshot final {
public:
    class Children final {
    public:
        class Iterator final {
        public:
            using value_type = EntityReference;
            using difference_type = std::ptrdiff_t;
            using iterator_category = std::forward_iterator_tag;
            Iterator() = default;
            [[nodiscard]] EntityReference operator*() const;
            Iterator& operator++();
            Iterator operator++(int);
            [[nodiscard]] friend bool operator==(const Iterator&, const Iterator&) = default;

        private:
            friend class Children;
            Iterator(std::shared_ptr<const NInternal::TopologyLease> lease, EntityReference entity);
            std::shared_ptr<const NInternal::TopologyLease> m_lease;
            EntityReference m_entity;
        };
        [[nodiscard]] Iterator begin() const;
        [[nodiscard]] Iterator end() const;

    private:
        friend class TopologySnapshot;
        Children(std::shared_ptr<const NInternal::TopologyLease> lease, EntityReference first);
        std::shared_ptr<const NInternal::TopologyLease> m_lease;
        EntityReference m_first;
    };

    [[nodiscard]] std::uint64_t GetEpoch() const noexcept;
    [[nodiscard]] std::uint64_t GetWorldId() const noexcept;
    [[nodiscard]] bool IsAlive(EntityReference entity) const noexcept;
    [[nodiscard]] EntityReference GetParent(EntityReference entity) const;
    [[nodiscard]] Children GetChildren(EntityReference entity) const;
    [[nodiscard]] bool IsAncestor(EntityReference ancestor, EntityReference descendant) const;
    [[nodiscard]] NMath::Mat4 GetLocalTransform(EntityReference entity) const;
    // Iterative serial reference, without cache or hidden writes.
    [[nodiscard]] NMath::Mat4 GetWorldTransform(EntityReference entity) const;

private:
    friend class World;
    explicit TopologySnapshot(std::shared_ptr<const NInternal::TopologyLease> lease);
    std::shared_ptr<const NInternal::TopologyLease> m_lease;
};

} // namespace NEcs
