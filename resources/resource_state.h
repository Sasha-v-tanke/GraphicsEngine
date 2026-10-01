#pragma once

namespace NResources {

enum class EResourceState {
    UNLOADED,
    LOADING,
    READY,
    FAILED,
    UNLOADING,
};

} // namespace NResources
