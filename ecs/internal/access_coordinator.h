#pragma once

#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include <GraphicsEngine/lib/common/wrapper/non_transferable.h>

namespace NCommon {
class TaskSystem;
}

namespace NEcs::NInternal {

struct DataDomainKey {
    std::uint64_t OwnerId = 0;
    std::uint64_t TypeId = 0;
    std::uint64_t Generation = 0;
    friend bool operator==(DataDomainKey, DataDomainKey) = default;
};

enum class EAccessScope {
    WHOLE_STORAGE,
    CHUNK,
    ENTITY,
    ENTITY_SET,
    RESOURCE,
    UNKNOWN,
};

// CHUNK is a resolved half-open range [Begin, Begin+Count) in the same
// stable index space as ENTITY/ENTITY_SET. Resource IDs use a separate space.
// Cross-space and UNKNOWN overlaps are conservative. Owner leases must prevent
// recycling IDs or changing this mapping until all operations are quiescent.
struct AccessScope {
    EAccessScope Kind = EAccessScope::WHOLE_STORAGE;
    std::uint64_t Begin = 0;
    std::uint64_t Count = 0;
    std::vector<std::uint64_t> Elements = {};
    friend bool operator==(const AccessScope&, const AccessScope&) = default;
};

enum class EAccessRequirement {
    READ,
    WRITE,
    EXCLUSIVE,
};

struct AccessFootprint {
    DataDomainKey Domain;
    AccessScope Scope;
    EAccessRequirement Requirement = EAccessRequirement::READ;
};

class OperationKey final {
public:
    OperationKey() = default;
    [[nodiscard]] std::uint64_t GetSequence() const noexcept {
        return m_sequence;
    }
    friend bool operator==(OperationKey, OperationKey) = default;

private:
    std::uint64_t m_owner = 0;
    std::uint64_t m_sequence = 0;
    friend class AccessCoordinator;
};

enum class EOperationStatus {
    WAITING,
    QUEUED,
    RUNNING,
    COMPLETED,
    FAILED,
    CANCELLED,
};

struct AccessConflict {
    OperationKey Predecessor;
    DataDomainKey Domain;
    AccessScope EarlierScope;
    AccessScope LaterScope;
};

// Internal owner-side scheduling only; does not grant arbitrary World access.
// All accesses in one domain must go through this coordinator. TaskSystem must
// outlive it. The host destroys/Stop()s the coordinator before owner teardown.
// User work executes outside metadata locks; waiting operations occupy no worker.
class AccessCoordinator final: public NCommon::NonTransferable {
public:
    explicit AccessCoordinator(NCommon::TaskSystem& tasks);
    ~AccessCoordinator();

    // Register stable owner/type storage and retain its lifetime token. Increasing
    // generation replaces a domain only at quiescence. Old generations are rejected.
    // Never mutate/recycle underlying storage through the token during active work.
    void RegisterDomain(DataDomainKey domain, const std::shared_ptr<const void>& lifetime);

    // Admission sequence defines deterministic conflict order. Dependencies must
    // already belong to this coordinator; forward references/cycles are impossible.
    // A failed/cancelled predecessor cancels downstream (no partial-state consumers).
    // The callback must complete all declared accesses before returning, including
    // any child work: detached jobs do not inherit access. Empty footprint = no access.
    [[nodiscard]] OperationKey Schedule(std::vector<AccessFootprint> footprints,
                                        std::function<void()> function,
                                        std::span<const OperationKey> dependencies = {});
    // Pending only: running work is never interrupted or loses its access lease.
    [[nodiscard]] bool Cancel(OperationKey operation);
    [[nodiscard]] EOperationStatus GetStatus(OperationKey operation) const;
    [[nodiscard]] std::exception_ptr GetError(OperationKey operation) const;
    [[nodiscard]] std::vector<OperationKey> GetDependencies(OperationKey operation) const;
    [[nodiscard]] std::vector<AccessConflict> GetConflicts(OperationKey operation) const;

    // Host-only: close admission, cancel queued/waiting work, drain running callbacks.
    // Idempotent. Does not stop/wait the shared TaskSystem or execute work on the host.
    void Stop();

private:
    struct State;
    [[nodiscard]] std::size_t Resolve(OperationKey operation) const;
    [[nodiscard]] OperationKey MakeKey(std::size_t index) const;
    std::shared_ptr<State> m_state;
    std::uint64_t m_owner = 0;
};

} // namespace NEcs::NInternal
