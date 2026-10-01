#include "resource_manager.h"

#include <atomic>
#include <limits>
#include <utility>

#include <lib/common/error/exception.h>

namespace NResources {

std::size_t ResourceIdentityHash::operator()(const ResourceIdentity& identity) const noexcept {
    const std::hash<std::string_view> hash;
    const std::size_t resourceClass = hash(identity.GetResourceClass());
    const std::size_t key = hash(identity.GetKey());

    return resourceClass ^ (key + 0x9e3779b97f4a7c15ULL + (resourceClass << 6U) + (resourceClass >> 2U));
}

ResourceManager::ResourceManager()
    : m_ownerId(AcquireOwnerId()) {
}

std::uint64_t ResourceManager::AcquireOwnerId() {
    static std::atomic_uint64_t nextOwnerId{1};

    const std::uint64_t ownerId = nextOwnerId.fetch_add(1, std::memory_order_relaxed);

    if (ownerId == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "ResourceManager owner id space is exhausted");
    }

    return ownerId;
}

ResourceManager::InternalHandle ResourceManager::Request(ResourceIdentity identity, std::type_index type) {
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

    ++entry.Generation;
    entry.Occupied = true;
    entry.Identity = std::move(identity);
    entry.Type = type;
    entry.State = EResourceState::UNLOADED;
    entry.CancelledOperationGeneration = 0;
    entry.CpuResource.reset();
    entry.Failure.reset();

    m_identityIndex.emplace(entry.Identity, slotIndex);

    return InternalHandle{
            .SlotIndex = slotIndex,
            .Generation = entry.Generation,
    };
}

std::uint64_t ResourceManager::BeginLoading(Entry& entry) {
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

    ++entry.OperationGeneration;
    entry.CancelledOperationGeneration = 0;
    entry.State = EResourceState::LOADING;
    entry.CpuResource.reset();
    entry.Failure.reset();

    return entry.OperationGeneration;
}

void ResourceManager::PublishReady(Entry& entry,
                                   std::uint64_t operationGeneration,
                                   std::shared_ptr<const void> resource) {
    if (!resource) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Ready resource payload must not be null");
    }

    if (entry.State != EResourceState::LOADING || entry.OperationGeneration != operationGeneration) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Resource '{}' has no matching loading operation",
                              entry.Identity.GetKey());
    }

    entry.CpuResource = std::move(resource);
    entry.Failure.reset();
    entry.State = EResourceState::READY;
}

void ResourceManager::Fail(Entry& entry, std::uint64_t operationGeneration, NCommon::ErrorInfo error) {
    if (entry.State != EResourceState::LOADING || entry.OperationGeneration != operationGeneration) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Resource '{}' has no matching loading operation",
                              entry.Identity.GetKey());
    }

    entry.CpuResource.reset();
    entry.Failure = std::move(error);
    entry.State = EResourceState::FAILED;
}

void ResourceManager::RequestUnload(Entry& entry) {
    if (entry.State != EResourceState::LOADING && entry.State != EResourceState::READY &&
        entry.State != EResourceState::FAILED) {
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

    if (entry.State == EResourceState::LOADING) {
        entry.CancelledOperationGeneration = entry.OperationGeneration;
    } else {
        entry.CancelledOperationGeneration = 0;
    }

    ++entry.OperationGeneration;
    entry.CpuResource.reset();
    entry.Failure.reset();
    entry.State = EResourceState::UNLOADING;
}

void ResourceManager::CompleteUnload(Entry& entry) {
    if (entry.State != EResourceState::UNLOADING) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Resource '{}' cannot complete unload from state {}",
                              entry.Identity.GetKey(),
                              static_cast<int>(entry.State));
    }

    entry.CpuResource.reset();
    entry.Failure.reset();
    entry.State = EResourceState::UNLOADED;
}

void ResourceManager::Forget(Entry& entry) {
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
    entry.CpuResource.reset();
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

} // namespace NResources
