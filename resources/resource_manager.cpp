#include "image_data.h"
#include "mesh_data.h"
#include "resource_manager.h"
#include "shader_artifact.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <map>
#include <type_traits>
#include <utility>

#include <lib/common/error/exception.h>

namespace NResources {

std::size_t ResourceIdentityHash::operator()(const ResourceIdentity& identity) const noexcept {
    const std::hash<std::string_view> hash;
    const std::size_t resourceClass = hash(identity.GetResourceClass());
    const std::size_t key = hash(identity.GetKey());

    return resourceClass ^ (key + 0x9e3779b97f4a7c15ULL + (resourceClass << 6U) + (resourceClass >> 2U));
}

std::size_t ResourceMemorySize<ImageData>::GetBytes(const ImageData& data) {
    return AddResourceMemory(sizeof(data), data.GetPixels().capacity(), sizeof(std::uint8_t));
}

std::size_t ResourceMemorySize<MeshData>::GetBytes(const MeshData& data) {
    auto bytes = AddResourceMemory(sizeof(data), data.GetVertices().capacity(), sizeof(MeshVertex));
    bytes = AddResourceMemory(bytes, data.GetIndices().capacity(), sizeof(std::uint32_t));
    return AddResourceMemory(bytes, data.GetAttributeMasks().capacity(), sizeof(std::uint8_t));
}

std::size_t ResourceMemorySize<ShaderArtifact>::GetBytes(const ShaderArtifact& data) {
    return AddResourceMemory(sizeof(data), data.GetWords().capacity(), sizeof(std::uint32_t));
}

ResourceManager::ResourceManager()
    : ResourceManager(ResourceMemoryBudget{}) {
}

ResourceManager::ResourceManager(ResourceMemoryBudget budget)
    : m_memoryBudget(budget)
    , m_ownerId(AcquireOwnerId()) {
}

std::uint64_t ResourceManager::AcquireOwnerId() {
    static std::atomic_uint64_t nextOwnerId{1};

    const std::uint64_t ownerId = nextOwnerId.fetch_add(1, std::memory_order_relaxed);

    if (ownerId == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "ResourceManager owner id space is exhausted");
    }

    return ownerId;
}

ResourceManager::InternalHandle
ResourceManager::Request(ResourceIdentity identity, std::type_index type, RetiredPayload& retired) {
    if (!identity.IsValid()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Resource identity must have class and key");
    }

    if (const auto it = m_identityIndex.find(identity); it != m_identityIndex.end()) {
        Entry& entry = m_entries[it->second];
        static_cast<void>(ValidateType(entry, type));

        return InternalHandle{
                .SlotIndex = it->second,
                .Generation = entry.Generation,
        };
    }

    std::size_t slotIndex = m_entries.size();

    for (std::size_t index = 0; index < m_entries.size(); ++index) {
        if (!m_entries[index].Occupied) {
            slotIndex = index;
            break;
        }
    }

    if (slotIndex == m_entries.size()) {
        m_entries.emplace_back();
    }

    Entry& entry = m_entries[slotIndex];

    if (entry.Generation == std::numeric_limits<std::uint64_t>::max()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Resource slot {} generation space is exhausted",
                              slotIndex);
    }

    // Prepare the index before publishing a slot. Failed vector growth or index insertion
    // leaves only an unoccupied slot; the commit below cannot throw or consume a generation.
    static_assert(std::is_nothrow_move_assignable_v<ResourceIdentity>);
    static_assert(std::is_nothrow_move_constructible_v<Entry>);
    m_identityIndex.emplace(identity, slotIndex);

    ++entry.Generation;
    entry.Occupied = true;
    entry.Identity = std::move(identity);
    entry.Type = type;
    entry.State = EResourceState::UNLOADED;
    entry.CancelledOperationGeneration = 0;
    entry.PublishedVersion = 0;
    RetirePayload(entry, retired);
    entry.Failure.reset();

    return InternalHandle{
            .SlotIndex = slotIndex,
            .Generation = entry.Generation,
    };
}

std::uint64_t ResourceManager::BeginLoading(Entry& entry, RetiredPayload& retired) {
    if (entry.State != EResourceState::UNLOADED && entry.State != EResourceState::FAILED) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Resource '{}' cannot begin loading from state {}",
                              entry.Identity.GetKey(),
                              static_cast<int>(entry.State));
    }

    if (entry.OperationGeneration == std::numeric_limits<std::uint64_t>::max()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Resource '{}' operation generation space is exhausted",
                              entry.Identity.GetKey());
    }

    std::stop_source cancellation;
    ++entry.OperationGeneration;
    entry.Cancellation = std::move(cancellation);
    entry.CancelledOperationGeneration = 0;
    entry.State = EResourceState::LOADING;
    RetirePayload(entry, retired);
    entry.Failure.reset();

    return entry.OperationGeneration;
}

void ResourceManager::PublishReady(Entry& entry,
                                   std::uint64_t operationGeneration,
                                   std::shared_ptr<const void>& resource,
                                   std::size_t bytes,
                                   ResourceMemoryReservation* reservation) {
    if (!resource) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Ready resource payload must not be null");
    }

    if (entry.State != EResourceState::LOADING || entry.OperationGeneration != operationGeneration) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Resource '{}' has no matching loading operation",
                              entry.Identity.GetKey());
    }

    if (entry.PublishedVersion == std::numeric_limits<std::uint64_t>::max()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Resource '{}' version space is exhausted",
                              entry.Identity.GetKey());
    }

    resource = TrackPayload(entry, resource, bytes, reservation);
    ++entry.PublishedVersion;
    entry.CpuResource.swap(resource);
    entry.Failure.reset();
    entry.State = EResourceState::READY;
    entry.Memory->Ready = true;
}

void ResourceManager::Fail(Entry& entry,
                           std::uint64_t operationGeneration,
                           NCommon::ErrorInfo error,
                           RetiredPayload& retired) {
    if (entry.State != EResourceState::LOADING || entry.OperationGeneration != operationGeneration) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Resource '{}' has no matching loading operation",
                              entry.Identity.GetKey());
    }

    RetirePayload(entry, retired);
    entry.Failure = std::move(error);
    entry.State = EResourceState::FAILED;
}

void ResourceManager::RequestUnload(Entry& entry) {
    if (!IsLoadingState(entry.State) && entry.State != EResourceState::READY && entry.State != EResourceState::FAILED) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Resource '{}' cannot begin unloading from state {}",
                              entry.Identity.GetKey(),
                              static_cast<int>(entry.State));
    }

    if (entry.OperationGeneration == std::numeric_limits<std::uint64_t>::max()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Resource '{}' operation generation space is exhausted",
                              entry.Identity.GetKey());
    }

    if (IsLoadingState(entry.State)) {
        entry.CancelledOperationGeneration = entry.OperationGeneration;
    } else {
        entry.CancelledOperationGeneration = 0;
    }

    ++entry.OperationGeneration;
    entry.Failure.reset();
    entry.State = EResourceState::UNLOADING;
}

void ResourceManager::CompleteUnload(Entry& entry, RetiredPayload& retired) {
    if (entry.State != EResourceState::UNLOADING) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Resource '{}' cannot complete unload from state {}",
                              entry.Identity.GetKey(),
                              static_cast<int>(entry.State));
    }

    RetirePayload(entry, retired);
    entry.Failure.reset();
    entry.State = EResourceState::UNLOADED;
}

void ResourceManager::Forget(Entry& entry, RetiredPayload& retired) {
    if (entry.State != EResourceState::UNLOADED) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Resource '{}' cannot be forgotten from state {}",
                              entry.Identity.GetKey(),
                              static_cast<int>(entry.State));
    }

    const ResourceIdentity identity = entry.Identity;

    m_identityIndex.erase(identity);

    entry.Occupied = false;
    entry.Identity = {};
    entry.Type = std::type_index{typeid(void)};
    entry.CancelledOperationGeneration = 0;
    entry.PublishedVersion = 0;
    RetirePayload(entry, retired);
    entry.Failure.reset();

    if (entry.Generation == std::numeric_limits<std::uint64_t>::max()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Resource generation space is exhausted");
    }

    ++entry.Generation;
}

EResourceState ResourceManager::GetState(const Entry& entry) noexcept {
    return entry.State;
}

ResourceIdentity ResourceManager::GetIdentity(const Entry& entry) {
    return entry.Identity;
}

std::optional<NCommon::ErrorInfo> ResourceManager::GetFailure(const Entry& entry) {
    return entry.Failure;
}

ResourceManager::Entry&
ResourceManager::ValidateUntyped(std::uint64_t ownerId, std::size_t slotIndex, std::uint64_t generation) {
    return const_cast<Entry&>(std::as_const(*this).ValidateUntyped(ownerId, slotIndex, generation));
}

const ResourceManager::Entry&
ResourceManager::ValidateUntyped(std::uint64_t ownerId, std::size_t slotIndex, std::uint64_t generation) const {
    if (ownerId == 0 || ownerId != m_ownerId) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Resource handle belongs to another ResourceManager");
    }

    if (slotIndex >= m_entries.size()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Resource handle slot is out of range");
    }

    const Entry& entry = m_entries[slotIndex];

    if (!entry.Occupied || entry.Generation != generation) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Stale resource handle");
    }

    return entry;
}

ResourceManager::Entry& ResourceManager::ValidateType(Entry& entry, std::type_index type) {
    return const_cast<Entry&>(ValidateType(static_cast<const Entry&>(entry), type));
}

const ResourceManager::Entry& ResourceManager::ValidateType(const Entry& entry, std::type_index type) {
    if (entry.Type != type) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                              "Resource '{}' was requested with another resource type",
                              entry.Identity.GetKey());
    }

    return entry;
}

void ResourceManager::SetMemoryBudget(ResourceMemoryBudget budget) {
    std::unique_lock lock{m_mutex};
    m_memoryBudget = budget;
}

ResourceMemorySnapshot ResourceManager::MemorySnapshot(const void* excluded) const {
    ResourceMemorySnapshot snapshot;
    snapshot.Budget = m_memoryBudget;
    snapshot.Total.HighWaterBytes = m_memoryTotalHighWater;
    std::map<std::string, ResourceMemoryCounters> classes;
    for (const auto& [name, highWater]: m_memoryHighWater) {
        classes[name].HighWaterBytes = highWater;
    }
    for (const auto& record: m_memoryRecords) {
        if (record->Lifetime.expired()) {
            continue;
        }
        // Reservations have an internal trivial token; locking it cannot run user destructors.
        if (record->Pending && record->Lifetime.lock().get() == excluded) {
            continue;
        }
        auto& counters = classes[record->ResourceClass];
        for (auto* target: {&counters, &snapshot.Total}) {
            if (record->Pending) {
                target->PendingDecodeBytes += record->Bytes;
            } else {
                target->DecodedBytes += record->Bytes;
                if (record->Cached) {
                    target->CacheBytes += record->Bytes;
                }
                if (!record->Cached || !record->Ready || record->Lifetime.use_count() > 1 ||
                    record->CachedPayload.use_count() > 1) {
                    target->PinnedBytes += record->Bytes;
                } else {
                    target->EvictableBytes += record->Bytes;
                }
            }
        }
    }
    for (auto& [name, counters]: classes) {
        snapshot.Classes.push_back({name, counters});
    }
    const auto used = snapshot.Total.DecodedBytes + snapshot.Total.PendingDecodeBytes;
    snapshot.AtCapacity = used >= snapshot.Budget.MaxCpuBytes;
    return snapshot;
}

ResourceMemorySnapshot ResourceManager::GetMemorySnapshot() const {
    std::shared_lock lock{m_mutex};
    return MemorySnapshot();
}

void ResourceManager::CheckMemoryAdmission(std::size_t bytes, const void* excluded) const {
    const auto snapshot = MemorySnapshot(excluded);
    const auto used = snapshot.Total.DecodedBytes + snapshot.Total.PendingDecodeBytes;
    if (used > m_memoryBudget.MaxCpuBytes || bytes > m_memoryBudget.MaxCpuBytes - used) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::OUT_OF_MEMORY,
                              "CPU resource budget exceeded: used={}, requested={}, limit={}",
                              used,
                              bytes,
                              m_memoryBudget.MaxCpuBytes);
    }
}

void ResourceManager::RecordMemory(const std::shared_ptr<MemoryRecord>& record, const void* excluded) {
    // Prepare every throwing operation before publishing a charge or changing a high-water value.
    const auto snapshot = MemorySnapshot(excluded);
    std::size_t classUsed = record->Bytes;
    for (const auto& item: snapshot.Classes) {
        if (item.ResourceClass == record->ResourceClass) {
            classUsed += item.Counters.DecodedBytes + item.Counters.PendingDecodeBytes;
        }
    }
    auto [highWater, inserted] = m_memoryHighWater.try_emplace(record->ResourceClass, 0);
    try {
        std::erase_if(m_memoryRecords, [](const auto& item) { return item->Lifetime.expired(); });
        m_memoryRecords.push_back(record);
    } catch (...) {
        if (inserted) {
            m_memoryHighWater.erase(highWater);
        }
        throw;
    }
    highWater->second = std::max(highWater->second, classUsed);
    m_memoryTotalHighWater = std::max(m_memoryTotalHighWater,
                                      snapshot.Total.DecodedBytes + snapshot.Total.PendingDecodeBytes + record->Bytes);
}

ResourceMemoryReservation ResourceManager::ReserveDecode(std::string resourceClass, std::size_t bytes) {
    if (resourceClass.empty()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Decode reservation needs a resource class");
    }
    ResourceMemoryReservation reservation;
    reservation.m_class = std::move(resourceClass);
    reservation.m_owner = m_ownerId;
    reservation.m_token = std::make_shared<int>(0);
    auto record = std::make_shared<MemoryRecord>();
    record->ResourceClass = reservation.m_class;
    record->Bytes = bytes;
    record->Pending = true;
    record->Lifetime = reservation.m_token;
    std::unique_lock lock{m_mutex};
    CheckMemoryAdmission(bytes, nullptr);
    RecordMemory(record);
    return reservation;
}

std::shared_ptr<const void> ResourceManager::TrackPayload(Entry& entry,
                                                          const std::shared_ptr<const void>& payload,
                                                          std::size_t bytes,
                                                          ResourceMemoryReservation* reservation) {
    if (reservation && (!reservation->IsValid() || reservation->m_owner != m_ownerId ||
                        reservation->m_class != entry.Identity.GetResourceClass())) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Foreign, consumed or wrong-class decode reservation");
    }
    const void* excluded = reservation ? reservation->m_token.get() : nullptr;
    CheckMemoryAdmission(bytes, excluded);
    // The original caller keeps payload alive across all throwing allocations under this lock.
    auto owner = std::make_shared<std::shared_ptr<const void>>(payload);
    std::shared_ptr<const void> tracked{std::move(owner), payload.get()};
    auto record = std::make_shared<MemoryRecord>();
    record->ResourceClass = entry.Identity.GetResourceClass();
    record->Bytes = bytes;
    record->Cached = true;
    record->Lifetime = tracked;
    record->CachedPayload = tracked;
    // Reservation conversion is atomic; RecordMemory must also exclude it from high-water sums.
    RecordMemory(record, excluded);
    if (reservation) {
        reservation->m_token.reset();
    }
    if (entry.Memory) {
        entry.Memory->Cached = false;
    }
    entry.Memory = std::move(record);
    return tracked;
}

} // namespace NResources
