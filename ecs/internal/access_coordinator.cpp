#include "access_coordinator.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <optional>
#include <tuple>
#include <utility>

#include <GraphicsEngine/lib/common/error/error.h>
#include <GraphicsEngine/lib/common/error/exception.h>
#include <GraphicsEngine/lib/thread/task/task_system.h>

namespace NEcs::NInternal {
namespace {
bool IsTerminal(EOperationStatus status) {
    return status == EOperationStatus::COMPLETED || status == EOperationStatus::FAILED ||
           status == EOperationStatus::CANCELLED;
}

void Normalize(AccessScope& scope) {
    switch (scope.Kind) {
    case EAccessScope::WHOLE_STORAGE:
    case EAccessScope::UNKNOWN:
        if (scope.Begin != 0 || scope.Count != 0 || !scope.Elements.empty()) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Unbounded scope has coordinates");
        }
        break;
    case EAccessScope::CHUNK:
        if (scope.Count == 0 || scope.Begin > std::numeric_limits<std::uint64_t>::max() - scope.Count ||
            !scope.Elements.empty()) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Invalid resolved chunk range");
        }
        break;
    case EAccessScope::ENTITY:
    case EAccessScope::RESOURCE:
        if (scope.Count != 0 || !scope.Elements.empty()) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Scalar scope has extra coordinates");
        }
        break;
    case EAccessScope::ENTITY_SET:
        if (scope.Begin != 0 || scope.Count != 0 || scope.Elements.empty()) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Invalid entity set scope");
        }
        std::ranges::sort(scope.Elements);
        scope.Elements.erase(std::unique(scope.Elements.begin(), scope.Elements.end()), scope.Elements.end());
        break;
    default:
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Unknown scope kind");
    }
}

bool Contains(const AccessScope& scope, std::uint64_t element) {
    switch (scope.Kind) {
    case EAccessScope::ENTITY:
        return scope.Begin == element;
    case EAccessScope::ENTITY_SET:
        return std::ranges::binary_search(scope.Elements, element);
    case EAccessScope::CHUNK:
        return element >= scope.Begin && element - scope.Begin < scope.Count;
    default:
        return true;
    }
}

bool Overlaps(const AccessScope& left, const AccessScope& right) {
    if (left.Kind == EAccessScope::WHOLE_STORAGE || right.Kind == EAccessScope::WHOLE_STORAGE ||
        left.Kind == EAccessScope::UNKNOWN || right.Kind == EAccessScope::UNKNOWN) {
        return true;
    }
    if (left.Kind == EAccessScope::RESOURCE || right.Kind == EAccessScope::RESOURCE) {
        return left.Kind != right.Kind || left.Begin == right.Begin;
    }
    if (left.Kind == EAccessScope::ENTITY) {
        return Contains(right, left.Begin);
    }
    if (right.Kind == EAccessScope::ENTITY) {
        return Contains(left, right.Begin);
    }
    if (left.Kind == EAccessScope::ENTITY_SET) {
        return std::ranges::any_of(left.Elements, [&](std::uint64_t element) { return Contains(right, element); });
    }
    if (right.Kind == EAccessScope::ENTITY_SET) {
        return Overlaps(right, left);
    }
    return left.Begin < right.Begin + right.Count && right.Begin < left.Begin + left.Count;
}

void Normalize(std::vector<AccessFootprint>& footprints) {
    for (AccessFootprint& footprint: footprints) {
        if (footprint.Domain.OwnerId == 0 || footprint.Domain.TypeId == 0 || footprint.Domain.Generation == 0) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Invalid data domain identity");
        }
        switch (footprint.Requirement) {
        case EAccessRequirement::READ:
        case EAccessRequirement::WRITE:
        case EAccessRequirement::EXCLUSIVE:
            break;
        default:
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Unknown access requirement");
        }
        Normalize(footprint.Scope);
    }
    std::ranges::sort(footprints, [](const AccessFootprint& left, const AccessFootprint& right) {
        return std::tie(left.Domain.OwnerId,
                        left.Domain.TypeId,
                        left.Domain.Generation,
                        left.Scope.Kind,
                        left.Scope.Begin,
                        left.Scope.Count,
                        left.Scope.Elements,
                        left.Requirement) < std::tie(right.Domain.OwnerId,
                                                     right.Domain.TypeId,
                                                     right.Domain.Generation,
                                                     right.Scope.Kind,
                                                     right.Scope.Begin,
                                                     right.Scope.Count,
                                                     right.Scope.Elements,
                                                     right.Requirement);
    });
    footprints.erase(std::unique(footprints.begin(),
                                 footprints.end(),
                                 [](const auto& left, const auto& right) {
                                     return left.Domain == right.Domain && left.Scope == right.Scope &&
                                            left.Requirement == right.Requirement;
                                 }),
                     footprints.end());
}
} // namespace

struct AccessCoordinator::State: std::enable_shared_from_this<State> {
    struct Domain {
        DataDomainKey Key;
        std::shared_ptr<const void> Lifetime;
    };
    struct Operation {
        std::vector<AccessFootprint> Footprints;
        std::shared_ptr<std::function<void()>> Function;
        std::vector<std::size_t> Dependencies;
        std::vector<AccessConflict> Conflicts;
        EOperationStatus Status = EOperationStatus::WAITING;
        std::exception_ptr Failure;
        bool IsRetiring = false;
    };
    static bool HoldsAccess(const Operation& operation) {
        return !IsTerminal(operation.Status) || operation.Function != nullptr || operation.IsRetiring;
    }
    explicit State(NCommon::TaskSystem& tasks)
        : Tasks(tasks) {
    }

    // Reserve READY publication under metadata lock; call the executor outside it.
    // Submit may retire unrelated payloads whose destructors reenter the coordinator.
    // Concurrent progress calls cannot publish the same QUEUED operation twice.
    void Progress() {
        for (;;) {
            std::optional<std::size_t> readyIndex;
            {
                const std::lock_guard lock{Mutex};
                for (std::size_t index = 0; index < Operations.size(); ++index) {
                    Operation& operation = Operations[index];
                    if (operation.Status != EOperationStatus::WAITING) {
                        continue;
                    }
                    bool ready = true;
                    for (const std::size_t predecessor: operation.Dependencies) {
                        const EOperationStatus status = Operations[predecessor].Status;
                        if (status == EOperationStatus::FAILED || status == EOperationStatus::CANCELLED) {
                            operation.Status = EOperationStatus::CANCELLED;
                            break;
                        }
                        ready = ready && status == EOperationStatus::COMPLETED;
                    }
                    if (operation.Status == EOperationStatus::WAITING && ready) {
                        operation.Status = EOperationStatus::QUEUED;
                        readyIndex = index;
                        break;
                    }
                }
            }
            if (!readyIndex) {
                return;
            }
            try {
                const std::weak_ptr<State> weak = weak_from_this();
                (void)Tasks.Submit([weak, index = *readyIndex](NCommon::TaskContext&) {
                    if (const std::shared_ptr<State> state = weak.lock()) {
                        state->Run(index);
                    }
                });
            } catch (...) {
                const std::lock_guard lock{Mutex};
                Operation& operation = Operations[*readyIndex];
                if (operation.Status == EOperationStatus::QUEUED) {
                    operation.Failure = std::current_exception();
                    HasFailure = true;
                    operation.Status = EOperationStatus::FAILED;
                }
            }
        }
    }

    // Destruction of arbitrary user captures never runs under metadata locks.
    void ReleaseTerminalFunctions() {
        for (;;) {
            std::shared_ptr<std::function<void()>> released;
            std::size_t retiredIndex = 0;
            {
                const std::lock_guard lock{Mutex};
                for (std::size_t index = 0; index < Operations.size(); ++index) {
                    Operation& operation = Operations[index];
                    if (IsTerminal(operation.Status) && operation.Function) {
                        released = std::move(operation.Function);
                        operation.IsRetiring = true;
                        retiredIndex = index;
                        ++Retiring;
                        break;
                    }
                }
            }
            if (!released) {
                return;
            }
            released.reset();
            {
                const std::lock_guard lock{Mutex};
                Operations[retiredIndex].IsRetiring = false;
                --Retiring;
                Changed.notify_all();
            }
        }
    }

    void Run(std::size_t index) {
        std::shared_ptr<std::function<void()>> function;
        {
            const std::lock_guard lock{Mutex};
            Operation& operation = Operations[index];
            if (operation.Status != EOperationStatus::QUEUED) {
                return;
            }
            operation.Status = EOperationStatus::RUNNING;
            ++Running;
            function = std::move(operation.Function);
        }
        std::exception_ptr failure;
        try {
            (*function)();
        } catch (...) {
            failure = std::current_exception();
        }
        function.reset();
        {
            const std::lock_guard lock{Mutex};
            Operation& operation = Operations[index];
            operation.Failure = failure;
            HasFailure = HasFailure || failure != nullptr;
            operation.Status = failure ? EOperationStatus::FAILED : EOperationStatus::COMPLETED;
            --Running;
            Changed.notify_all();
        }
        Progress();
        ReleaseTerminalFunctions();
    }

    NCommon::TaskSystem& Tasks;
    std::mutex Mutex;
    std::condition_variable Changed;
    std::vector<Domain> Domains;
    std::vector<Operation> Operations;
    std::size_t Running = 0;
    std::size_t Retiring = 0;
    bool Closed = false;
    bool HasFailure = false;
};

AccessCoordinator::AccessCoordinator(NCommon::TaskSystem& tasks)
    : m_state(std::make_shared<State>(tasks)) {
    static std::atomic<std::uint64_t> nextOwner{1};
    std::uint64_t owner = nextOwner.load();
    for (;;) {
        if (owner == std::numeric_limits<std::uint64_t>::max()) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Access coordinator identity exhausted");
        }
        if (nextOwner.compare_exchange_weak(owner, owner + 1)) {
            m_owner = owner;
            break;
        }
    }
}
AccessCoordinator::~AccessCoordinator() {
    Stop();
}

std::size_t AccessCoordinator::Resolve(OperationKey operation) const {
    if (operation.m_owner != m_owner || operation.m_sequence == 0 ||
        operation.m_sequence > m_state->Operations.size()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Unknown/foreign operation key");
    }
    return static_cast<std::size_t>(operation.m_sequence - 1);
}
OperationKey AccessCoordinator::MakeKey(std::size_t index) const {
    OperationKey key;
    key.m_owner = m_owner;
    key.m_sequence = index + 1;
    return key;
}

void AccessCoordinator::RegisterDomain(DataDomainKey domain, const std::shared_ptr<const void>& lifetime) {
    if (domain.OwnerId == 0 || domain.TypeId == 0 || domain.Generation == 0 || !lifetime) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Invalid data domain registration");
    }
    std::shared_ptr<const void> retired;
    {
        const std::lock_guard lock{m_state->Mutex};
        if (m_state->Closed || m_state->HasFailure) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Access coordinator stopped or failed");
        }
        for (State::Domain& existing: m_state->Domains) {
            if (existing.Key.OwnerId != domain.OwnerId || existing.Key.TypeId != domain.TypeId) {
                continue;
            }
            if (domain.Generation <= existing.Key.Generation) {
                GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Domain generation must increase");
            }
            for (const State::Operation& operation: m_state->Operations) {
                if (State::HoldsAccess(operation) && std::ranges::any_of(operation.Footprints, [&](const auto& access) {
                        return access.Domain == existing.Key;
                    })) {
                    GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Domain is still in use");
                }
            }
            retired = std::move(existing.Lifetime);
            existing = {domain, lifetime};
            return;
        }
        m_state->Domains.push_back({domain, lifetime});
    }
}

OperationKey AccessCoordinator::Schedule(std::vector<AccessFootprint> footprints,
                                         std::function<void()> function,
                                         std::span<const OperationKey> dependencies) {
    if (!function) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Empty access operation");
    }
    Normalize(footprints);
    State::Operation operation;
    operation.Footprints = std::move(footprints);
    operation.Function = std::make_shared<std::function<void()>>(std::move(function));
    OperationKey key;
    {
        const std::lock_guard lock{m_state->Mutex};
        if (m_state->Closed || m_state->HasFailure) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Access coordinator stopped or failed");
        }
        for (const AccessFootprint& footprint: operation.Footprints) {
            if (!std::ranges::any_of(m_state->Domains,
                                     [&](const State::Domain& domain) { return domain.Key == footprint.Domain; })) {
                GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Stale/unregistered access domain");
            }
        }
        for (const OperationKey dependency: dependencies) {
            operation.Dependencies.push_back(Resolve(dependency));
        }
        for (std::size_t index = 0; index < m_state->Operations.size(); ++index) {
            const State::Operation& earlier = m_state->Operations[index];
            if (!State::HoldsAccess(earlier)) {
                continue;
            }
            for (const AccessFootprint& left: earlier.Footprints) {
                for (const AccessFootprint& right: operation.Footprints) {
                    if (left.Domain == right.Domain &&
                        (left.Requirement != EAccessRequirement::READ ||
                         right.Requirement != EAccessRequirement::READ) &&
                        Overlaps(left.Scope, right.Scope)) {
                        operation.Dependencies.push_back(index);
                        operation.Conflicts.push_back({MakeKey(index), left.Domain, left.Scope, right.Scope});
                    }
                }
            }
        }
        std::ranges::sort(operation.Dependencies);
        operation.Dependencies.erase(std::unique(operation.Dependencies.begin(), operation.Dependencies.end()),
                                     operation.Dependencies.end());
        if (m_state->Operations.size() == std::numeric_limits<std::uint64_t>::max()) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Operation sequence exhausted");
        }
        key = MakeKey(m_state->Operations.size());
        m_state->Operations.push_back(std::move(operation));
    }
    m_state->Progress();
    m_state->ReleaseTerminalFunctions();
    return key;
}

bool AccessCoordinator::Cancel(OperationKey operation) {
    {
        const std::lock_guard lock{m_state->Mutex};
        State::Operation& record = m_state->Operations[Resolve(operation)];
        if (record.Status != EOperationStatus::WAITING && record.Status != EOperationStatus::QUEUED) {
            return false;
        }
        record.Status = EOperationStatus::CANCELLED;
    }
    m_state->Progress();
    m_state->ReleaseTerminalFunctions();
    return true;
}
EOperationStatus AccessCoordinator::GetStatus(OperationKey operation) const {
    const std::lock_guard lock{m_state->Mutex};
    return m_state->Operations[Resolve(operation)].Status;
}
std::exception_ptr AccessCoordinator::GetError(OperationKey operation) const {
    const std::lock_guard lock{m_state->Mutex};
    return m_state->Operations[Resolve(operation)].Failure;
}
std::vector<OperationKey> AccessCoordinator::GetDependencies(OperationKey operation) const {
    const std::lock_guard lock{m_state->Mutex};
    std::vector<OperationKey> result;
    for (const std::size_t index: m_state->Operations[Resolve(operation)].Dependencies) {
        result.push_back(MakeKey(index));
    }
    return result;
}
std::vector<AccessConflict> AccessCoordinator::GetConflicts(OperationKey operation) const {
    const std::lock_guard lock{m_state->Mutex};
    return m_state->Operations[Resolve(operation)].Conflicts;
}
void AccessCoordinator::Stop() {
    if (NCommon::TaskSystem::IsCurrentThreadWorker()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Access coordinator shutdown requires host thread");
    }
    {
        const std::lock_guard lock{m_state->Mutex};
        m_state->Closed = true;
        for (State::Operation& operation: m_state->Operations) {
            if (operation.Status != EOperationStatus::RUNNING && !IsTerminal(operation.Status)) {
                operation.Status = EOperationStatus::CANCELLED;
            }
        }
    }
    m_state->ReleaseTerminalFunctions();
    {
        std::unique_lock lock{m_state->Mutex};
        m_state->Changed.wait(lock, [&] { return m_state->Running == 0 && m_state->Retiring == 0; });
    }
    // Release owner tokens on the stopping host, outside the metadata lock. Queued
    // executor wrappers capture weak State only and can no longer invoke user work.
    for (;;) {
        std::shared_ptr<const void> retired;
        {
            const std::lock_guard lock{m_state->Mutex};
            for (State::Domain& domain: m_state->Domains) {
                if (domain.Lifetime) {
                    retired = std::move(domain.Lifetime);
                    break;
                }
            }
        }
        if (!retired) {
            break;
        }
    }
}

} // namespace NEcs::NInternal
