#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include <GraphicsEngine/lib/thread/job.h>
#include <engine/engine_config.h>
#include <lib/common/error/error.h>
#include <lib/common/wrapper/non_transferable.h>

namespace NEngine {

namespace NRuntime {

class EngineFactory;

}

enum class EEngineState {
    CREATED,
    RUNNING,
    STOPPING,
    STOPPED,
};

// Non-owning identity of a reserved frame. Run/owner and slot generation prevent
// delayed external completion from admitting work into a recycled frame or a new Run.
class FrameKey final {
public:
    [[nodiscard]] bool IsValid() const noexcept {
        return m_owner != 0;
    }
    [[nodiscard]] std::uint64_t GetRunGeneration() const noexcept {
        return m_runGeneration;
    }
    [[nodiscard]] std::uint64_t GetFrameSequence() const noexcept {
        return m_frameSequence;
    }
    [[nodiscard]] std::size_t GetSlotIndex() const noexcept {
        return m_slotIndex;
    }
    [[nodiscard]] std::uint64_t GetGeneration() const noexcept {
        return m_generation;
    }
    [[nodiscard]] friend bool operator==(FrameKey, FrameKey) noexcept = default;

private:
    std::uint64_t m_owner = 0;
    std::uint64_t m_runGeneration = 0;
    std::uint64_t m_frameSequence = 0;
    std::size_t m_slotIndex = 0;
    std::uint64_t m_generation = 0;
    friend class Engine;
};

class Engine final: private NCommon::NonTransferable {
public:
    explicit Engine(EngineConfig config = {});
    ~Engine();

    void Start();

    void Stop() noexcept;

    // Internal Application facade handoff; admission is serialized with Stop.
    void SubmitUserJob(std::function<void()> function);

    // Independent user work on the existing executor; never linked to frame checkpoints or storage.
    // Admission requires RUNNING. Stop drains accepted work; cooperative cancellation is a later layer.
    template<NCommon::UserJobFunction Function>
    [[nodiscard]] auto RunAsync(Function&& function) {
        return NCommon::NJobInternal::JobFactory::Run(
                [this](std::function<void()> task) { SubmitUserJob(std::move(task)); },
                std::forward<Function>(function));
    }

    // Reserves physical storage before publishing user work. No worker task is
    // queued until CompleteUserUpdate. Nullopt is backpressure, not a reservation.
    [[nodiscard]] std::optional<FrameKey> TryBeginFrame();
    // Completion is exactly once. Stale/duplicate/stopped signals return false.
    [[nodiscard]] bool CompleteUserUpdate(FrameKey frame);
    // Only an uncompleted user reservation may be aborted; never cancels live CPU/GPU users.
    [[nodiscard]] bool AbortFrame(FrameKey frame);

    [[nodiscard]] std::uint64_t GetProgressVersion() const;
    // Main/orchestration only; bounded wait keeps platform/GPU maintenance live.
    // Capture version before attempting admission to avoid lost notifications.
    void WaitForProgress(std::uint64_t version, std::chrono::milliseconds timeout) const;

    bool Update();

    bool Draw();

    [[nodiscard]] bool CanAcceptUpdate() const;

    [[nodiscard]] std::uint64_t GetGpuRecycledFrameCount() const;

    [[nodiscard]] EEngineState GetState() const noexcept;

    [[nodiscard]] std::optional<NCommon::ErrorInfo> GetLastError() const;

    void ClearLastError();

private:
    class Impl;

    explicit Engine(std::unique_ptr<Impl> impl);

private:
    std::unique_ptr<Impl> m_impl;

    friend class NRuntime::EngineFactory;
};

} // namespace NEngine
