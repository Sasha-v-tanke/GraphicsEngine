#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include <GraphicsEngine/lib/common/error/error.h>
#include <GraphicsEngine/lib/common/wrapper/non_transferable.h>
#include <GraphicsEngine/resources/resource_handle.h>
#include <GraphicsEngine/resources/resource_identity.h>
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
        std::unique_lock lock{m_mutex};
        RequestUnload(Validate(handle, std::type_index{typeid(T)}));
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

        if (entry.State != EResourceState::READY) {
            return {};
        }

        return std::static_pointer_cast<const T>(entry.CpuResource);
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
        std::shared_ptr<const void> CpuResource;
        std::optional<NCommon::ErrorInfo> Failure;
    };

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
