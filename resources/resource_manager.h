#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include <GraphicsEngine/lib/common/error/error.h>
#include <GraphicsEngine/lib/common/wrapper/non_transferable.h>
#include <GraphicsEngine/resources/resource_handle.h>
#include <GraphicsEngine/resources/resource_identity.h>
#include <GraphicsEngine/resources/resource_lease.h>
#include <GraphicsEngine/resources/resource_state.h>

namespace NResources {

class ResourceManager final: private NCommon::NonTransferable {
public:
    ResourceManager();

    template<typename T>
    [[nodiscard]] ResourceHandle<T> Request(ResourceIdentity identity) {
        std::unique_lock lock{m_mutex};
        return MakeTypedHandle<T>(Request(std::move(identity), std::type_index{typeid(T)}));
    }

    template<typename T>
    [[nodiscard]] ResourceOperation<T> BeginLoading(ResourceHandle<T> handle) {
        std::unique_lock lock{m_mutex};
        return ResourceOperation<T>{
                handle,
                BeginLoading(Validate(handle, std::type_index{typeid(T)})),
        };
    }

    // Atomic admission: concurrent pipelines coalesce at the logical identity.
    template<typename T>
    [[nodiscard]] std::optional<ResourceOperation<T>> TryQueueLoading(ResourceHandle<T> handle) {
        std::unique_lock lock{m_mutex};
        Entry& entry = Validate(handle, std::type_index{typeid(T)});
        if (entry.State != EResourceState::UNLOADED && entry.State != EResourceState::FAILED) {
            return std::nullopt;
        }
        const auto generation = BeginLoading(entry);
        entry.State = EResourceState::QUEUED;
        return ResourceOperation<T>{handle, generation};
    }

    template<typename T>
    [[nodiscard]] bool IsOperationCurrent(ResourceOperation<T> operation) const {
        std::shared_lock lock{m_mutex};
        return FindOperation(operation) != nullptr;
    }

    template<typename T>
    bool TryAdvance(ResourceOperation<T> operation,
                    EResourceState from,
                    EResourceState to,
                    std::shared_ptr<const T> cpu = {},
                    std::shared_ptr<const void> gpu = {}) {
        // The alias keeps both immutable representations alive in leases/use records.
        struct Payload {
            std::shared_ptr<const T> Cpu;
            std::shared_ptr<const void> Gpu;
        };
        std::weak_ptr<const void> gpuVersion = gpu;
        std::shared_ptr<const void> retained;
        if (gpu && cpu) {
            auto payload = std::make_shared<Payload>(cpu, std::move(gpu));
            retained = std::shared_ptr<const T>{std::move(payload), cpu.get()};
        } else {
            retained = std::move(cpu);
        }
        std::unique_lock lock{m_mutex};
        Entry* entry = const_cast<Entry*>(FindOperation(operation));
        if (!entry || entry->State != from) {
            return false;
        }
        const bool valid = (from == EResourceState::QUEUED && to == EResourceState::LOADING) ||
                           (from == EResourceState::LOADING && to == EResourceState::CPU_READY && retained) ||
                           (from == EResourceState::CPU_READY && to == EResourceState::UPLOADING) ||
                           ((from == EResourceState::CPU_READY || from == EResourceState::UPLOADING) &&
                            to == EResourceState::READY && (retained || entry->CpuResource));
        if (!valid) {
            return false;
        }
        if (to == EResourceState::READY && entry->PublishedVersion == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("Resource version space exhausted");
        }
        if (retained) {
            entry->CpuResource.swap(retained);
        }
        if (to == EResourceState::READY) {
            ++entry->PublishedVersion;
            entry->GpuResource = std::move(gpuVersion);
        }
        entry->State = to;
        lock.unlock();
        return true;
    }

    template<typename T>
    bool TryFail(ResourceOperation<T> operation, NCommon::ErrorInfo error) {
        std::shared_ptr<const void> retired;
        std::unique_lock lock{m_mutex};
        Entry* entry = const_cast<Entry*>(FindOperation(operation));
        if (!entry || !IsLoadingState(entry->State)) {
            return false;
        }
        retired.swap(entry->CpuResource);
        entry->Failure = std::move(error);
        entry->State = EResourceState::FAILED;
        lock.unlock();
        return true;
    }

    // Cancels admission even when its pipeline work record has not been registered yet.
    template<typename T>
    [[nodiscard]] std::optional<ResourceOperation<T>> TryCancelLoading(ResourceHandle<T> handle) {
        std::shared_ptr<const void> retired;
        std::unique_lock lock{m_mutex};
        Entry& entry = Validate(handle, std::type_index{typeid(T)});
        if (!IsLoadingState(entry.State)) {
            return std::nullopt;
        }
        const ResourceOperation<T> operation{handle, entry.OperationGeneration};
        entry.CancelledOperationGeneration = entry.OperationGeneration;
        retired.swap(entry.CpuResource);
        entry.Failure.reset();
        entry.State = EResourceState::UNLOADED;
        lock.unlock();
        return operation;
    }

    template<typename T>
    bool TryCancel(ResourceOperation<T> operation) {
        std::shared_ptr<const void> retired;
        std::unique_lock lock{m_mutex};
        Entry* entry = const_cast<Entry*>(FindOperation(operation));
        if (!entry || !IsLoadingState(entry->State)) {
            return false;
        }
        entry->CancelledOperationGeneration = entry->OperationGeneration;
        retired.swap(entry->CpuResource);
        entry->Failure.reset();
        entry->State = EResourceState::UNLOADED;
        lock.unlock();
        return true;
    }

    template<typename T>
    void PublishReady(ResourceOperation<T> operation, std::shared_ptr<T> resource) {
        std::unique_lock lock{m_mutex};
        PublishReady(Validate(operation.GetResource(), std::type_index{typeid(T)}),
                     operation.GetGeneration(),
                     std::move(resource));
    }

    template<typename T>
    void Fail(ResourceOperation<T> operation, NCommon::ErrorInfo error) {
        std::unique_lock lock{m_mutex};
        Fail(Validate(operation.GetResource(), std::type_index{typeid(T)}),
             operation.GetGeneration(),
             std::move(error));
    }

    template<typename T>
    void RequestUnload(ResourceHandle<T> handle) {
        std::shared_ptr<const void> retired;
        std::unique_lock lock{m_mutex};
        Entry& entry = Validate(handle, std::type_index{typeid(T)});
        RequestUnload(entry);
        retired.swap(entry.CpuResource);
        lock.unlock();
    }

    template<typename T>
    void CompleteUnload(ResourceHandle<T> handle) {
        std::unique_lock lock{m_mutex};
        CompleteUnload(Validate(handle, std::type_index{typeid(T)}));
    }

    template<typename T>
    void Forget(ResourceHandle<T> handle) {
        std::unique_lock lock{m_mutex};
        Forget(Validate(handle, std::type_index{typeid(T)}));
    }

    template<typename T>
    [[nodiscard]] EResourceState GetState(ResourceHandle<T> handle) const {
        std::shared_lock lock{m_mutex};
        return GetState(Validate(handle, std::type_index{typeid(T)}));
    }

    template<typename T>
    [[nodiscard]] ResourceIdentity GetIdentity(ResourceHandle<T> handle) const {
        std::shared_lock lock{m_mutex};
        return GetIdentity(Validate(handle, std::type_index{typeid(T)}));
    }

    template<typename T>
    [[nodiscard]] std::shared_ptr<const T> GetCpuResource(ResourceHandle<T> handle) const {
        std::shared_lock lock{m_mutex};
        const Entry& entry = Validate(handle, std::type_index{typeid(T)});

        if (entry.State != EResourceState::READY && entry.State != EResourceState::CPU_READY &&
            entry.State != EResourceState::UPLOADING) {
            return {};
        }

        return std::static_pointer_cast<const T>(entry.CpuResource);
    }

    template<typename T>
    [[nodiscard]] std::optional<ResourceLease<T>> TryAcquire(ResourceHandle<T> handle) const {
        if (!handle.IsValid()) {
            return std::nullopt;
        }

        std::shared_lock lock{m_mutex};
        const Entry& entry = ValidateUntyped(handle.m_ownerId, handle.m_slotIndex, handle.m_generation);

        if (entry.State != EResourceState::READY || !entry.CpuResource) {
            return std::nullopt;
        }

        return ResourceLease<T>{
                handle,
                entry.PublishedVersion,
                std::static_pointer_cast<const T>(entry.CpuResource),
                entry.GpuResource.lock(),
        };
    }

    template<typename T>
    [[nodiscard]] std::optional<NCommon::ErrorInfo> GetFailure(ResourceHandle<T> handle) const {
        std::shared_lock lock{m_mutex};
        return GetFailure(Validate(handle, std::type_index{typeid(T)}));
    }

    template<typename T>
    [[nodiscard]] bool IsCancellationRequested(ResourceOperation<T> operation) const {
        std::shared_lock lock{m_mutex};
        const Entry& entry = Validate(operation.GetResource(), std::type_index{typeid(T)});

        return entry.CancelledOperationGeneration == operation.GetGeneration();
    }

private:
    struct Entry {
        bool Occupied = false;
        ResourceIdentity Identity;
        std::type_index Type = std::type_index{typeid(void)};
        EResourceState State = EResourceState::UNLOADED;
        std::uint64_t Generation = 0;
        std::uint64_t OperationGeneration = 0;
        std::uint64_t CancelledOperationGeneration = 0;
        std::uint64_t PublishedVersion = 0;
        std::shared_ptr<const void> CpuResource;
        std::weak_ptr<const void> GpuResource;
        std::optional<NCommon::ErrorInfo> Failure;
    };

    static bool IsLoadingState(EResourceState state) noexcept {
        return state == EResourceState::QUEUED || state == EResourceState::LOADING ||
               state == EResourceState::CPU_READY || state == EResourceState::UPLOADING;
    }

    template<typename T>
    const Entry* FindOperation(ResourceOperation<T> operation) const {
        const auto handle = operation.GetResource();
        if (handle.m_ownerId != m_ownerId || handle.m_slotIndex >= m_entries.size()) {
            return nullptr;
        }
        const Entry& entry = m_entries[handle.m_slotIndex];
        if (!entry.Occupied || entry.Generation != handle.m_generation || entry.Type != typeid(T) ||
            entry.OperationGeneration != operation.GetGeneration() || !IsLoadingState(entry.State)) {
            return nullptr;
        }
        return &entry;
    }

    struct InternalHandle {
        std::size_t SlotIndex = 0;
        std::uint64_t Generation = 0;
    };

    [[nodiscard]] static std::uint64_t AcquireOwnerId();

    [[nodiscard]] InternalHandle Request(ResourceIdentity identity, std::type_index type);

    [[nodiscard]] static std::uint64_t BeginLoading(Entry& entry);

    static void PublishReady(Entry& entry, std::uint64_t operationGeneration, std::shared_ptr<const void> resource);

    static void Fail(Entry& entry, std::uint64_t operationGeneration, NCommon::ErrorInfo error);

    static void RequestUnload(Entry& entry);

    static void CompleteUnload(Entry& entry);

    void Forget(Entry& entry);

    [[nodiscard]] static EResourceState GetState(const Entry& entry) noexcept;

    [[nodiscard]] static ResourceIdentity GetIdentity(const Entry& entry);

    [[nodiscard]] static std::optional<NCommon::ErrorInfo> GetFailure(const Entry& entry);

    [[nodiscard]] Entry& ValidateUntyped(std::uint64_t ownerId, std::size_t slotIndex, std::uint64_t generation);

    [[nodiscard]] const Entry&
    ValidateUntyped(std::uint64_t ownerId, std::size_t slotIndex, std::uint64_t generation) const;

    [[nodiscard]] static Entry& ValidateType(Entry& entry, std::type_index type);

    [[nodiscard]] static const Entry& ValidateType(const Entry& entry, std::type_index type);

    template<typename T>
    [[nodiscard]] ResourceHandle<T> MakeTypedHandle(InternalHandle handle) const noexcept {
        return ResourceHandle<T>{
                m_ownerId,
                handle.SlotIndex,
                handle.Generation,
        };
    }

    template<typename T>
    [[nodiscard]] Entry& Validate(ResourceHandle<T> handle, std::type_index type) {
        if (!handle.IsValid()) {
            return ValidateUntyped(0, handle.m_slotIndex, handle.m_generation);
        }

        return ValidateType(ValidateUntyped(handle.m_ownerId, handle.m_slotIndex, handle.m_generation), type);
    }

    template<typename T>
    [[nodiscard]] const Entry& Validate(ResourceHandle<T> handle, std::type_index type) const {
        if (!handle.IsValid()) {
            return ValidateUntyped(0, handle.m_slotIndex, handle.m_generation);
        }

        return ValidateType(ValidateUntyped(handle.m_ownerId, handle.m_slotIndex, handle.m_generation), type);
    }

private:
    mutable std::shared_mutex m_mutex;
    std::uint64_t m_ownerId = 0;
    std::vector<Entry> m_entries;
    std::unordered_map<ResourceIdentity, std::size_t, ResourceIdentityHash> m_identityIndex;
};

} // namespace NResources
