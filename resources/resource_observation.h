#pragma once

#include <optional>

#include <GraphicsEngine/lib/common/error/error.h>
#include <GraphicsEngine/resources/resource_identity.h>
#include <GraphicsEngine/resources/resource_lease.h>
#include <GraphicsEngine/resources/resource_state.h>

namespace NResources {

enum class EResourceReferenceStatus {
    VALID,
    EMPTY,
    FOREIGN,
    STALE,
    TYPE_MISMATCH,
};

// Value snapshot: identity/failure strings remain valid after unload, Forget or retry.
// Identity and state are absent when the requested generation cannot be identified safely.
struct ResourceStatus {
    EResourceReferenceStatus Reference = EResourceReferenceStatus::EMPTY;
    std::optional<ResourceIdentity> Identity;
    std::optional<EResourceState> State;
    std::optional<NCommon::ErrorInfo> Failure;
};

template<typename T>
struct ResourceObservation {
    ResourceStatus Status;
    std::optional<ResourceLease<T>> Lease;
};

} // namespace NResources
