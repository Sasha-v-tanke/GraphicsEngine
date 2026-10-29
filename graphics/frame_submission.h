#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <vector>

#include <GraphicsEngine/graphics/completion_point.h>
#include <GraphicsEngine/lib/common/resource_use_record.h>

namespace NGraphics {

struct FrameSubmission {
    std::uint64_t FrameIndex = 0;
    std::size_t FrameSlotIndex = 0;
    bool RequiresPresentation = true;
    std::vector<NCommon::ResourceUseRecord> ResourceUseRecords;
    std::array<float, 4> ClearColor{0.1F, 0.2F, 0.4F, 1.0F};
};

enum class EFrameSubmissionStatus {
    PRESENTED,
    NOT_READY,
    OUT_OF_DATE,
    SUSPENDED,
    FAILED,
};

struct FrameSubmissionResult {
    EFrameSubmissionStatus Status = EFrameSubmissionStatus::NOT_READY;
    // Valid even when presentation fails after an accepted submit.
    CompletionPoint Completion;
    std::exception_ptr Failure = {};
};

} // namespace NGraphics
