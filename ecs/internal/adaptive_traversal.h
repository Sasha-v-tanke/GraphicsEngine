#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include <GraphicsEngine/lib/common/error/exception.h>
#include <GraphicsEngine/lib/common/wrapper/non_transferable.h>
#include <GraphicsEngine/lib/thread/task/task_system.h>
#include <ecs/internal/topology.h>

namespace NEcs::NInternal {

// Internal access to the pinned storage, without copying hierarchy links per run.
class TraversalTopologyAccess {
public:
    static const TopologyState& Get(const TopologySnapshot& topology) {
        return *topology.m_lease->State;
    }
};

enum class ETraversalMode {
    TOP_DOWN,
    BOTTOM_UP,
    INDEPENDENT,
};

struct BatchPolicy {
    std::size_t MaxPacketsPerWorker = 2;
    std::size_t TargetPacketsPerWorker = 4;
    std::chrono::nanoseconds MinimumPacketTime{100000};
    double InitialNanosecondsPerWorkUnit = 1000;
};

struct TraversalMetrics {
    std::size_t VisitedNodes = 0;
    std::size_t Packets = 0;
    std::size_t PeakActivePackets = 0;
    std::uint64_t SchedulingNanoseconds = 0;
    std::uint64_t WorkNanoseconds = 0;
    double TargetWorkUnits = 0;
};

template<typename Result>
struct TraversalInput {
    EntityReference Entity;
    const Result* ParentResult = nullptr;
    std::span<const Result* const> ChildResults;
};

namespace NTraversal {
struct Feedback {
    std::atomic<double> NanosecondsPerUnit{0};
    std::atomic<double> SchedulingNanoseconds{0};
};

template<typename Result>
struct Run;
} // namespace NTraversal

template<typename Result>
class TraversalResults;

// One-shot publication slot. TaskSystem must outlive scheduling/observation.
// Values are accessible only after COMPLETED, never after failure/cancellation.
// Results must own their data; borrowed component references are not access leases.
template<typename Result>
class TraversalOutput final: public NCommon::NonTransferable {
public:
    [[nodiscard]] std::optional<TraversalResults<Result>> TryAcquire(const NCommon::TaskSystem& tasks) const;
    [[nodiscard]] TraversalMetrics GetMetrics() const;

private:
    friend class AdaptiveBatchPlanner;
    mutable std::mutex m_mutex;
    std::shared_ptr<NTraversal::Run<Result>> m_run;
    NCommon::TaskGroupHandle m_group;
};

template<typename Result>
struct TraversalOperation {
    // TOP_DOWN: immutable parent result (nullptr at selected roots).
    // BOTTOM_UP: child results in topology sibling order, parent is nullptr.
    // INDEPENDENT: both dependency inputs are empty. Each entity has one writer.
    std::function<Result(TraversalInput<Result>)> Visit;
    std::shared_ptr<TraversalOutput<Result>> Output;
    // Optional pure estimate in positive finite units. Evaluated before publication.
    std::function<double(EntityReference)> EstimateWork = {};
    // Optional parent-first, ancestor-closed selection relative to roots. Siblings
    // follow selection order; omitted subtrees are not visited or evaluated.
    std::optional<std::vector<EntityReference>> SelectedNodes = {};
};

namespace NTraversal {
using Clock = std::chrono::steady_clock;
inline std::uint64_t Nanoseconds(Clock::duration duration) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
}

template<typename Result>
struct Run: std::enable_shared_from_this<Run<Result>> {
    struct Node {
        EntityReference Identity;
        EntityReference FirstChild;
        EntityReference LastChild;
        EntityReference NextSibling;
        bool IsRoot = false;
        std::optional<Result> Value;
        double OwnCost = 0;
        double SubtreeCost = 0;
        std::atomic_size_t RemainingChildren{0};
    };
    struct WorkItem {
        EntityReference First = {};
        EntityReference End = {};
        double Cost = 0;
        std::size_t RootBegin = 0;
        std::size_t RootEnd = 0;
    };
    struct Packet {
        std::shared_ptr<Run> Owner;
        WorkItem Work;
        ~Packet() {
            Owner->Active.fetch_sub(1);
            Owner->Retire();
        }
    };
    std::optional<TopologySnapshot> Topology;
    const TopologyState* Storage = nullptr;
    std::unique_ptr<Node[]> Nodes;
    std::size_t SlotCount = 0;
    std::uint64_t WorldId = 0;
    std::vector<EntityReference> Roots;
    std::function<Result(TraversalInput<Result>)> Visit;
    ETraversalMode Mode = ETraversalMode::INDEPENDENT;
    BatchPolicy Policy;
    std::shared_ptr<Feedback> History;
    std::size_t MaxPackets = 1;
    double BaseTarget = 1;
    double InitialRate = 1;
    double TotalCost = 0;
    std::atomic<double> LiveRate{1};
    std::atomic_size_t Outstanding{1}; // Admission owns the initial participant, released before SealGroup.
    std::atomic_size_t Active{0};
    std::atomic_size_t Peak{0};
    std::atomic_size_t PacketCount{0};
    std::atomic_size_t Visited{0};
    std::atomic<std::uint64_t> SchedulingTime{0};
    std::atomic<std::uint64_t> WorkTime{0};
    std::atomic<double> CompletedUnits{0};
    std::atomic<double> Target{1};

    void Retire() noexcept {
        if (Outstanding.fetch_sub(1) != 1) {
            return;
        }
        // Every packet capture has retired. Drop callbacks and topology before
        // the last TaskGroup participant publishes terminal completion.
        Visit = {};
        Topology.reset();
        Storage = nullptr;
        if (History) {
            const double units = CompletedUnits.load();
            if (units > 0) {
                History->NanosecondsPerUnit.store(static_cast<double>(WorkTime.load()) / units);
            }
            const auto packets = PacketCount.load();
            if (packets != 0) {
                History->SchedulingNanoseconds.store(static_cast<double>(SchedulingTime.load()) /
                                                     static_cast<double>(packets));
            }
        }
    }
    [[nodiscard]] double GetTarget() const {
        const double rate = std::max(1.0, LiveRate.load());
        const double overhead = History ? History->SchedulingNanoseconds.load() : 0;
        return std::max(
                {1.0, BaseTarget, static_cast<double>(Policy.MinimumPacketTime.count()) / rate, overhead * 8.0 / rate});
    }
    bool ReservePacket() {
        std::size_t active = Active.load();
        while (active < MaxPackets) {
            if (Active.compare_exchange_weak(active, active + 1)) {
                std::size_t peak = Peak.load();
                while (peak < active + 1 && !Peak.compare_exchange_weak(peak, active + 1)) {
                }
                return true;
            }
        }
        return false;
    }
    std::shared_ptr<Packet> MakePacket(WorkItem work) {
        auto packet = std::make_shared<Packet>();
        packet->Work = work;
        packet->Owner = this->shared_from_this();
        Outstanding.fetch_add(1);
        return packet;
    }
    bool Spawn(NCommon::TaskContext& context, WorkItem work) {
        if (!ReservePacket()) {
            return false;
        }
        std::shared_ptr<Packet> packet;
        try {
            packet = MakePacket(work);
        } catch (...) {
            Active.fetch_sub(1);
            throw;
        }
        const auto started = Clock::now();
        (void)context.SpawnInGroup([packet = std::move(packet)](NCommon::TaskContext& child) {
            packet->Owner->Execute(packet->Work, &child);
        });
        SchedulingTime.fetch_add(Nanoseconds(Clock::now() - started));
        PacketCount.fetch_add(1);
        return true;
    }
    bool HasWork(const WorkItem& work) const {
        return work.RootEnd != 0 ? work.RootBegin < work.RootEnd : work.First != work.End;
    }
    EntityReference Front(const WorkItem& work) const {
        return work.RootEnd != 0 ? Roots[work.RootBegin] : work.First;
    }
    void Advance(WorkItem& work) const {
        if (work.RootEnd != 0) {
            ++work.RootBegin;
        } else {
            work.First = Nodes[work.First.Identity.Index].NextSibling;
        }
    }
    bool Split(WorkItem& work, NCommon::TaskContext* context) {
        if (!context || MaxPackets == 1 || Active.load() >= MaxPackets) {
            return false;
        }
        const double target = GetTarget();
        Target.store(target);
        if (work.Cost < target * 2) {
            return false;
        }
        auto remaining = work;
        double prefixCost = 0;
        do {
            prefixCost += Nodes[Front(remaining).Identity.Index].SubtreeCost;
            Advance(remaining);
        } while (HasWork(remaining) && prefixCost < target);
        if (!HasWork(remaining)) {
            return false; // No independent remainder, including every bamboo chain.
        }
        auto prefix = work;
        prefix.Cost = prefixCost;
        if (work.RootEnd != 0) {
            prefix.RootEnd = remaining.RootBegin;
        } else {
            prefix.End = remaining.First;
        }
        if (!Spawn(*context, prefix)) {
            return false;
        }
        remaining.Cost = std::max(0.0, work.Cost - prefixCost);
        work = remaining;
        return true;
    }
    void Compute(EntityReference entity, std::vector<const Result*>& children, double& units, std::size_t& visited) {
        auto& node = Nodes[entity.Identity.Index];
        const Result* parentValue = nullptr;
        if (Mode == ETraversalMode::TOP_DOWN) {
            const auto parent = Storage->Get(entity).Parent;
            if (parent != EntityReference{} && Nodes[parent.Identity.Index].Identity == parent) {
                parentValue = &*Nodes[parent.Identity.Index].Value;
            }
        }
        children.clear();
        if (Mode == ETraversalMode::BOTTOM_UP) {
            for (auto child = Nodes[entity.Identity.Index].FirstChild; child != EntityReference{};
                 child = Nodes[child.Identity.Index].NextSibling) {
                children.push_back(&*Nodes[child.Identity.Index].Value);
            }
        }
        const auto sampleStarted = units == 0 ? Clock::now() : Clock::time_point{};
        node.Value.emplace(Visit({entity, parentValue, children}));
        if (units == 0) {
            LiveRate.store(
                    std::max(1.0, static_cast<double>(Nanoseconds(Clock::now() - sampleStarted)) / node.OwnCost));
        }
        ++visited;
        units += node.OwnCost;
    }
    void Reduce(EntityReference entity,
                std::vector<const Result*>& children,
                double& units,
                std::size_t& visited,
                NCommon::TaskContext* context) {
        for (;;) {
            if (context && context->IsGroupCancellationRequested()) {
                return;
            }
            Compute(entity, children, units, visited);
            const auto parent = Storage->Get(entity).Parent;
            if (parent == EntityReference{} || Nodes[parent.Identity.Index].Identity != parent ||
                Nodes[parent.Identity.Index].RemainingChildren.fetch_sub(1) != 1) {
                return;
            }
            // The final child acquires all sibling publications. Continue the
            // reduction iteratively, in topology order, without a waiting worker.
            entity = parent;
        }
    }
    void Execute(WorkItem work, NCommon::TaskContext* context) {
        const auto started = Clock::now();
        double units = 0;
        std::size_t visited = 0;
        const auto record = [&] {
            WorkTime.fetch_add(Nanoseconds(Clock::now() - started));
            CompletedUnits.fetch_add(units);
            Visited.fetch_add(visited);
        };
        try {
            std::vector<WorkItem> local{work};
            std::vector<const Result*> children;
            while (!local.empty()) {
                if (context && context->IsGroupCancellationRequested()) {
                    break;
                }
                auto item = local.back();
                local.pop_back();
                if (Split(item, context)) {
                    local.push_back(item);
                    continue;
                }
                const auto entity = Front(item);
                Advance(item);
                item.Cost = std::max(0.0, item.Cost - Nodes[entity.Identity.Index].SubtreeCost);
                if (HasWork(item)) {
                    local.push_back(item);
                }
                if (Mode == ETraversalMode::BOTTOM_UP) {
                    if (Nodes[entity.Identity.Index].FirstChild == EntityReference{}) {
                        Reduce(entity, children, units, visited, context);
                    }
                } else {
                    Compute(entity, children, units, visited);
                }
                const auto first = Nodes[entity.Identity.Index].FirstChild;
                if (first != EntityReference{}) {
                    local.push_back({first,
                                     {},
                                     Nodes[entity.Identity.Index].SubtreeCost - Nodes[entity.Identity.Index].OwnCost});
                }
            }
        } catch (...) {
            record();
            throw;
        }
        record();
    }
};

template<typename Result>
std::shared_ptr<Run<Result>> Prepare(TopologySnapshot topology,
                                     std::span<const EntityReference> roots,
                                     ETraversalMode mode,
                                     TraversalOperation<Result>& operation,
                                     const BatchPolicy& policy,
                                     std::size_t workers,
                                     std::shared_ptr<Feedback> history) {
    if (!operation.Visit || !operation.Output ||
        (mode != ETraversalMode::TOP_DOWN && mode != ETraversalMode::BOTTOM_UP &&
         mode != ETraversalMode::INDEPENDENT) ||
        policy.MaxPacketsPerWorker == 0 || policy.TargetPacketsPerWorker == 0 || policy.MinimumPacketTime.count() < 0 ||
        !std::isfinite(policy.InitialNanosecondsPerWorkUnit) || policy.InitialNanosecondsPerWorkUnit <= 0 ||
        policy.MaxPacketsPerWorker > std::numeric_limits<std::size_t>::max() / workers ||
        policy.TargetPacketsPerWorker > std::numeric_limits<std::size_t>::max() / workers) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Invalid traversal operation or batch policy");
    }
    auto run = std::make_shared<Run<Result>>();
    run->Topology.emplace(std::move(topology));
    run->Storage = &TraversalTopologyAccess::Get(*run->Topology);
    run->SlotCount = run->Storage->Nodes.size();
    run->WorldId = run->Topology->GetWorldId();
    run->Nodes = std::make_unique<typename Run<Result>::Node[]>(run->SlotCount);
    run->Roots.assign(roots.begin(), roots.end());
    run->Visit = std::move(operation.Visit);
    run->Mode = mode;
    run->Policy = policy;
    run->History = std::move(history);
    const double learned = run->History ? run->History->NanosecondsPerUnit.load() : 0;
    run->InitialRate = learned > 0 ? learned : policy.InitialNanosecondsPerWorkUnit;
    run->LiveRate.store(run->InitialRate);
    std::vector<EntityReference> order;
    const auto initialize = [&](EntityReference entity) -> typename Run<Result>::Node& {
        (void)run->Storage->Get(entity);
        auto& node = run->Nodes[entity.Identity.Index];
        if (node.Identity != EntityReference{}) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Traversal nodes overlap or repeat");
        }
        node.Identity = entity;
        node.OwnCost = operation.EstimateWork ? operation.EstimateWork(entity) : 1.0;
        if (!std::isfinite(node.OwnCost) || node.OwnCost <= 0) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                                  "Traversal work estimate must be positive and finite");
        }
        node.SubtreeCost = node.OwnCost;
        order.push_back(entity);
        return node;
    };
    if (operation.SelectedNodes) {
        for (const auto root: roots) {
            (void)run->Storage->Get(root);
            auto& node = run->Nodes[root.Identity.Index];
            if (node.IsRoot) {
                GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Traversal roots repeat");
            }
            node.IsRoot = true;
        }
        for (const auto entity: *operation.SelectedNodes) {
            auto& node = initialize(entity);
            const auto parent = run->Storage->Get(entity).Parent;
            const bool selectedParent =
                    parent != EntityReference{} && run->Nodes[parent.Identity.Index].Identity == parent;
            if (node.IsRoot) {
                if (selectedParent) {
                    GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Traversal roots overlap");
                }
            } else {
                if (!selectedParent) {
                    GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                                          "Traversal selection must be parent-first and ancestor-closed");
                }
                auto& parentNode = run->Nodes[parent.Identity.Index];
                if (parentNode.LastChild == EntityReference{}) {
                    parentNode.FirstChild = entity;
                } else {
                    run->Nodes[parentNode.LastChild.Identity.Index].NextSibling = entity;
                }
                parentNode.LastChild = entity;
                parentNode.RemainingChildren.fetch_add(1);
            }
        }
        for (const auto root: roots) {
            if (run->Nodes[root.Identity.Index].Identity != root) {
                GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Traversal root is outside selection");
            }
            const auto parent = run->Storage->Get(root).Parent;
            if (parent != EntityReference{} && run->Nodes[parent.Identity.Index].Identity == parent) {
                GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Traversal roots overlap");
            }
        }
    } else {
        std::vector<EntityReference> pending(roots.rbegin(), roots.rend());
        while (!pending.empty()) {
            const auto entity = pending.back();
            pending.pop_back();
            auto& node = initialize(entity);
            const auto& topologyNode = run->Storage->Get(entity);
            node.FirstChild = topologyNode.FirstChild;
            node.NextSibling = topologyNode.NextSibling;
            std::size_t children = 0;
            for (auto child = topologyNode.LastChild; child != EntityReference{};
                 child = run->Storage->Get(child).PreviousSibling) {
                pending.push_back(child);
                ++children;
            }
            node.RemainingChildren.store(children);
        }
    }
    double total = 0;
    for (const auto entity: std::views::reverse(order)) {
        const auto parent = run->Storage->Get(entity).Parent;
        if (parent != EntityReference{} && run->Nodes[parent.Identity.Index].Identity == parent) {
            auto& cost = run->Nodes[parent.Identity.Index].SubtreeCost;
            cost += run->Nodes[entity.Identity.Index].SubtreeCost;
            if (!std::isfinite(cost)) {
                GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Traversal subtree cost overflow");
            }
        }
    }
    for (const auto root: roots) {
        total += run->Nodes[root.Identity.Index].SubtreeCost;
    }
    if (!std::isfinite(total)) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Traversal forest cost overflow");
    }
    run->BaseTarget = total / static_cast<double>(workers * policy.TargetPacketsPerWorker);
    run->TotalCost = total;
    run->MaxPackets =
            workers == 1 ? 1 : std::max(std::size_t{1}, std::min(order.size(), workers * policy.MaxPacketsPerWorker));
    run->Target.store(run->GetTarget());
    operation.EstimateWork = {};
    return run;
}
} // namespace NTraversal

template<typename Result>
class TraversalResults {
public:
    [[nodiscard]] const Result& Get(EntityReference entity) const {
        if (entity.WorldId != m_run->WorldId || entity.Identity.Index >= m_run->SlotCount ||
            m_run->Nodes[entity.Identity.Index].Identity != entity || !m_run->Nodes[entity.Identity.Index].Value) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Entity is outside the completed traversal");
        }
        return *m_run->Nodes[entity.Identity.Index].Value;
    }

private:
    friend class TraversalOutput<Result>;
    friend class AdaptiveBatchPlanner;
    explicit TraversalResults(std::shared_ptr<const NTraversal::Run<Result>> run)
        : m_run(std::move(run)) {
    }
    std::shared_ptr<const NTraversal::Run<Result>> m_run;
};

template<typename Result>
std::optional<TraversalResults<Result>> TraversalOutput<Result>::TryAcquire(const NCommon::TaskSystem& tasks) const {
    const std::lock_guard lock{m_mutex};
    if (!m_run || tasks.GetGroupStatus(m_group) != NCommon::ETaskGroupStatus::COMPLETED) {
        return std::nullopt;
    }
    return TraversalResults<Result>{m_run};
}
template<typename Result>
TraversalMetrics TraversalOutput<Result>::GetMetrics() const {
    const std::lock_guard lock{m_mutex};
    if (!m_run) {
        return {};
    }
    return {m_run->Visited.load(),
            m_run->PacketCount.load(),
            m_run->Peak.load(),
            m_run->SchedulingTime.load(),
            m_run->WorkTime.load(),
            m_run->Target.load()};
}

// Own admission/access to component data separately (e.g. AccessCoordinator).
// The topology lease pins only hierarchy/local matrices, not arbitrary World data.
// Callbacks may run concurrently and must write disjoint outputs or return owned values.
class AdaptiveBatchPlanner final: public NCommon::NonTransferable {
public:
    explicit AdaptiveBatchPlanner(NCommon::TaskSystem& tasks)
        : m_tasks(tasks)
        , m_history(std::make_shared<NTraversal::Feedback>()) {
    }
    template<typename Result>
    [[nodiscard]] NCommon::TaskGroupHandle ScheduleTraversal(TopologySnapshot topology,
                                                             std::span<const EntityReference> roots,
                                                             ETraversalMode mode,
                                                             TraversalOperation<Result> operation,
                                                             const BatchPolicy& policy = BatchPolicy()) {
        auto run = NTraversal::Prepare(std::move(topology),
                                       roots,
                                       mode,
                                       operation,
                                       policy,
                                       m_tasks.GetWorkerCount(),
                                       m_history);
        NCommon::TaskGroupHandle group;
        {
            const std::lock_guard lock{operation.Output->m_mutex};
            if (operation.Output->m_run) {
                GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Traversal output is already bound");
            }
            group = m_tasks.CreateTaskGroup();
            operation.Output->m_run = run;
            operation.Output->m_group = group;
        }
        try {
            if (!roots.empty()) {
                // One admission packet handles the root forest and can split descendants.
                (void)run->ReservePacket();
                std::shared_ptr<typename NTraversal::Run<Result>::Packet> packet;
                try {
                    packet = run->MakePacket({.Cost = run->TotalCost, .RootEnd = roots.size()});
                } catch (...) {
                    run->Active.fetch_sub(1);
                    throw;
                }
                const auto started = NTraversal::Clock::now();
                (void)m_tasks.SubmitToGroup(group, [packet = std::move(packet)](NCommon::TaskContext& context) {
                    packet->Owner->Execute(packet->Work, &context);
                });
                run->SchedulingTime.fetch_add(NTraversal::Nanoseconds(NTraversal::Clock::now() - started));
                run->PacketCount.fetch_add(1);
            }
        } catch (...) {
            run->Retire();
            m_tasks.CancelGroup(group);
            throw;
        }
        run->Retire();
        m_tasks.SealGroup(group);
        return group;
    }
    template<typename Result>
    [[nodiscard]] static TraversalResults<Result> RunSerial(TopologySnapshot topology,
                                                            std::span<const EntityReference> roots,
                                                            ETraversalMode mode,
                                                            TraversalOperation<Result> operation) {
        auto run = NTraversal::Prepare(std::move(topology), roots, mode, operation, BatchPolicy(), 1, nullptr);
        if (!roots.empty()) {
            run->Execute({.Cost = run->TotalCost, .RootEnd = roots.size()}, nullptr);
        }
        run->Retire();
        return TraversalResults<Result>{std::move(run)};
    }

private:
    NCommon::TaskSystem& m_tasks;
    std::shared_ptr<NTraversal::Feedback> m_history;
};

} // namespace NEcs::NInternal
