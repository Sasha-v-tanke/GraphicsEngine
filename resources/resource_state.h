#pragma once

namespace NResources {

enum class EResourceState {
    UNLOADED,
    LOADING,
    READY,
    FAILED,
    UNLOADING,
    QUEUED,
    CPU_READY,
    UPLOADING,
};

} // namespace NResources
