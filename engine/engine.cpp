#include "engine.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <engine/controller/frame_scheduler.h>
#include <engine/runtime/frame_runtime.h>
#include <lib/common/error/exception.h>
#include <lib/thread/task/task_system.h>

namespace NEngine {

namespace {

[[nodiscard]] NCommon::ErrorInfo MakeRuntimeError(const std::exception_ptr& error) noexcept {
    try {
        if (error == nullptr) {
            return {
                    .Code = NCommon::make_error_code(NCommon::EError::UNKNOWN),
                    .Message = "Unknown engine runtime failure",
            };
        }

        try {
            std::rethrow_exception(error);
        } catch (const NCommon::Exception& exception) {
            return {
                    .Code = exception.code(),
                    .Message = exception.GetMessage(),
            };
        } catch (const std::system_error& exception) {
            return {
                    .Code = exception.code(),
                    .Message = exception.what(),
            };
        } catch (const std::exception& exception) {
            return {
                    .Code = NCommon::make_error_code(NCommon::EError::UNKNOWN),
                    .Message = exception.what(),
            };
        } catch (...) {
            return {
                    .Code = NCommon::make_error_code(NCommon::EError::UNKNOWN),
                    .Message = "Unknown non-standard engine runtime failure",
            };
        }
    } catch (...) {
        return {
                .Code = NCommon::make_error_code(NCommon::EError::OUT_OF_MEMORY),
                .Message = {},
        };
    }
}

[[nodiscard]] std::uint64_t AcquireEngineIdentity() {
    static std::atomic<std::uint64_t> next{1};
    std::uint64_t value = next.load(std::memory_order_relaxed);
    do {
        if (value == std::numeric_limits<std::uint64_t>::max()) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::OUT_OF_MEMORY, "Engine identity space exhausted");
        }
    } while (!next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed));
    return value;
}

[[nodiscard]] bool IsTerminalTaskStatus(NCommon::ETaskStatus status) noexcept {
    return status == NCommon::ETaskStatus::COMPLETED || status == NCommon::ETaskStatus::FAILED ||
           status == NCommon::ETaskStatus::CANCELLED;
}

} // namespace

namespace NRuntime {

class DefaultFrameRuntime final: public IFrameRuntime {
public:
    void Update(const FrameContext&) override {
    }

    void Draw(const FrameContext&) override {
    }
};

std::unique_ptr<IFrameRuntime> MakeDefaultFrameRuntime() {
    return std::make_unique<DefaultFrameRuntime>();
}

} // namespace NRuntime

class Engine::Impl final: private NCommon::NonTransferable {
public:
    Impl(EngineConfig config, std::unique_ptr<NRuntime::IFrameRuntime> frameRuntime)
        : m_config(config)
        , m_frameRuntime(std::move(frameRuntime)) {
        if (m_frameRuntime == nullptr) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Engine frame runtime is null");
        }
    }

    ~Impl() {
        Stop();
    }

    void Start() {
        std::lock_guard lock{m_mutex};

        if (m_state != EEngineState::CREATED && m_state != EEngineState::STOPPED) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Engine cannot start from state {}", ToInt(m_state));
        }

        m_lastError.reset();

        try {
            if (m_lifecycleGeneration == std::numeric_limits<std::uint64_t>::max()) {
                GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Engine run generation space exhausted");
            }
            m_taskSystem = std::make_unique<NCommon::TaskSystem>(m_config.WorkerCount);
            m_frameScheduler = std::make_unique<NController::FrameScheduler>(m_config);
            m_frameRecords = std::make_unique<FrameRuntimeRecord[]>(m_frameScheduler->GetMaxActiveFrames());
            m_nextDrawFrameIndex = 0;
            m_gpuRecycledFrameCount = 0;
            m_updateAdmissionOpen = true;
            ++m_lifecycleGeneration;
            m_lifecycleGenerationSnapshot.store(m_lifecycleGeneration, std::memory_order_release);
            m_state = EEngineState::RUNNING;
            NotifyProgressLocked();
        } catch (...) {
            RollbackStartLocked();
            throw;
        }
    }

    void SubmitUserJob(std::function<void()> function) {
        // Preserve an outer owner through submission rollback/rejection. User captures must
        // not be destroyed under the engine lock, even if executor admission throws.
        const std::shared_ptr<std::function<void()>> execution =
                std::make_shared<std::function<void()>>(std::move(function));
        std::lock_guard lock{m_mutex};
        if (m_state != EEngineState::RUNNING || !m_taskSystem) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "User jobs require a running Engine");
        }
        static_cast<void>(m_taskSystem->Submit([execution](NCommon::TaskContext&) {
            std::function<void()> ownedFunction = std::move(*execution);
            ownedFunction();
        }));
    }

    void Stop() noexcept {
        const std::uint64_t observedGeneration = m_lifecycleGenerationSnapshot.load(std::memory_order_acquire);
        std::unique_ptr<NCommon::TaskSystem> taskSystem;

        {
            std::unique_lock lock{m_mutex};

            if (m_state == EEngineState::CREATED || m_state == EEngineState::STOPPED) {
                m_state = EEngineState::STOPPED;
                m_stopCondition.notify_all();
                return;
            }

            if (m_stopInProgress) {
                m_stopCondition.wait(lock, [this, observedGeneration] {
                    return !m_stopInProgress || m_completedStopGeneration >= observedGeneration;
                });
                return;
            }

            if (m_lifecycleGeneration != observedGeneration) {
                return;
            }

            m_stopInProgress = true;
            m_activeStopGeneration = observedGeneration;
            m_state = EEngineState::STOPPING;
            NotifyProgressLocked();
            taskSystem = std::move(m_taskSystem);
        }

        try {
            if (taskSystem != nullptr) {
                taskSystem->WaitIdle();
            }
        } catch (...) {
            SetLastError(MakeRuntimeError(std::current_exception()));
        }

        m_frameRuntime->Drain();

        if (m_frameRecords != nullptr) {
            for (std::size_t index = 0; index < m_frameScheduler->GetMaxActiveFrames(); ++index) {
                if (!m_frameRecords[index].HasUpdateTask) {
                    continue;
                }
                const auto completion = m_frameScheduler->GetGpuCompletion(m_frameRecords[index].Frame);
                if (completion.valid()) {
                    completion.wait();
                }
            }
        }

        {
            std::lock_guard lock{m_mutex};

            if (taskSystem != nullptr) {
                ReapTerminalFrameRecordsLocked(*taskSystem);
            }
            AbortUnresolvedFrameRecordsLocked();
            ClearFrameRecordsLocked();
            m_frameScheduler.reset();
            m_frameRecords.reset();
            taskSystem.reset();
            m_state = EEngineState::STOPPED;
            m_updateAdmissionOpen = true;
            m_stopInProgress = false;
            m_completedStopGeneration = m_activeStopGeneration;
            m_stopCondition.notify_all();
            NotifyProgressLocked();
        }
    }

    [[nodiscard]] std::optional<FrameKey> TryBeginFrame() {
        std::lock_guard lock{m_mutex};
        return TryBeginFrameLocked();
    }

    [[nodiscard]] bool CompleteUserUpdate(FrameKey key) {
        std::lock_guard lock{m_mutex};
        return CompleteUserUpdateLocked(key);
    }

    [[nodiscard]] bool AbortFrame(FrameKey key) {
        std::lock_guard lock{m_mutex};
        FrameRuntimeRecord* record = ResolveReservationLocked(key);
        if (record == nullptr || record->HasUpdateTask) {
            return false;
        }
        m_frameScheduler->AbortFrame(record->Frame);
        m_nextDrawFrameIndex = record->Frame.GetFrameIndex() + 1;
        *record = {};
        m_updateAdmissionOpen = true;
        NotifyProgressLocked();
        return true;
    }

    [[nodiscard]] std::uint64_t GetProgressVersion() const {
        std::lock_guard lock{m_mutex};
        return m_progressVersion;
    }

    void WaitForProgress(std::uint64_t version, std::chrono::milliseconds timeout) const {
        if (NCommon::TaskSystem::IsCurrentThreadWorker()) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "A worker cannot wait for frame admission");
        }
        std::unique_lock lock{m_mutex};
        m_progressCondition.wait_for(lock, timeout, [this, version] {
            return m_progressVersion != version || m_state != EEngineState::RUNNING;
        });
    }

    bool Update() {
        std::lock_guard lock{m_mutex};
        RequireRunningLocked("update");
        const FrameRuntimeRecord& pending =
                m_frameRecords[static_cast<std::size_t>(m_nextDrawFrameIndex % m_frameScheduler->GetMaxActiveFrames())];
        if (pending.HasReservation && !pending.HasDrawTask && pending.HasUpdateTask) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Application update checkpoint is out of order");
        }
        const std::optional<FrameKey> key = TryBeginFrameLocked();
        return key.has_value() && CompleteUserUpdateLocked(*key);
    }

    bool Draw() {
        std::lock_guard lock{m_mutex};
        RequireRunningLocked("draw");

        FrameRuntimeRecord& record =
                m_frameRecords[static_cast<std::size_t>(m_nextDrawFrameIndex % m_frameScheduler->GetMaxActiveFrames())];

        if (!record.HasUpdateTask || record.Frame.GetFrameIndex() != m_nextDrawFrameIndex) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Application draw checkpoint is out of order");
        }

        const NController::FrameHandle frame = record.Frame;
        const NCommon::TaskHandle updateTask = record.UpdateTask;

        try {
            m_updateAdmissionOpen = false;
            m_frameScheduler->SignalDraw(frame);

            const NCommon::TaskHandle dependencies[] = {updateTask};

            record.DrawTask = m_taskSystem->Submit([this, frame](NCommon::TaskContext& task) { RunDraw(frame, task); },
                                                   dependencies);
            record.HasDrawTask = true;
        } catch (...) {
            SetLastErrorLocked(MakeRuntimeError(std::current_exception()));
            m_state = EEngineState::STOPPING;
            m_updateAdmissionOpen = true;

            try {
                m_frameScheduler->AbortFrame(frame);
            } catch (...) {
            }

            throw;
        }

        ++m_nextDrawFrameIndex;

        return true;
    }

    [[nodiscard]] bool CanAcceptUpdate() {
        std::lock_guard lock{m_mutex};
        RequireRunningLocked("accept update");
        ReapTerminalFrameRecordsLocked(*m_taskSystem);
        RequireRunningLocked("accept update");

        return m_updateAdmissionOpen;
    }

    [[nodiscard]] std::uint64_t GetGpuRecycledFrameCount() const {
        std::lock_guard lock{m_mutex};
        return m_gpuRecycledFrameCount;
    }

    [[nodiscard]] EEngineState GetState() const noexcept {
        std::lock_guard lock{m_mutex};

        return m_state;
    }

    [[nodiscard]] std::optional<NCommon::ErrorInfo> GetLastError() const {
        std::lock_guard lock{m_mutex};

        return m_lastError;
    }

    void ClearLastError() {
        std::lock_guard lock{m_mutex};

        m_lastError.reset();
    }

private:
    struct FrameRuntimeRecord {
        NController::FrameHandle Frame;
        FrameKey Key;
        bool HasReservation = false;
        NCommon::TaskHandle UpdateTask;
        NCommon::TaskHandle AsyncTask;
        NCommon::TaskHandle DrawTask;
        bool HasUpdateTask = false;
        bool HasDrawTask = false;
    };

    void NotifyProgressLocked() {
        ++m_progressVersion;
        m_progressCondition.notify_all();
    }

    [[nodiscard]] FrameRuntimeRecord* ResolveReservationLocked(FrameKey key) {
        if (m_state != EEngineState::RUNNING || !key.IsValid() || key.m_owner != m_ownerIdentity ||
            key.m_runGeneration != m_lifecycleGeneration || key.m_slotIndex >= m_config.MaxActiveFrames) {
            return nullptr;
        }
        FrameRuntimeRecord& record = m_frameRecords[key.m_slotIndex];
        return record.HasReservation && record.Key == key ? &record : nullptr;
    }

    [[nodiscard]] std::optional<FrameKey> TryBeginFrameLocked() {
        RequireRunningLocked("begin frame");
        ReapTerminalFrameRecordsLocked(*m_taskSystem);
        RequireRunningLocked("begin frame");
        if (!m_updateAdmissionOpen) {
            return std::nullopt;
        }
        const std::optional<NController::FrameHandle> frame = m_frameScheduler->TryAcquireFrame();
        if (!frame) {
            return std::nullopt;
        }
        FrameKey key;
        key.m_owner = m_ownerIdentity;
        key.m_runGeneration = m_lifecycleGeneration;
        key.m_frameSequence = frame->GetFrameIndex();
        key.m_slotIndex = frame->GetSlotIndex();
        key.m_generation = frame->GetGeneration();
        FrameRuntimeRecord& record = GetFrameRecordLocked(*frame);
        record.Frame = *frame;
        record.Key = key;
        record.HasReservation = true;
        m_updateAdmissionOpen = false;
        return key;
    }

    [[nodiscard]] bool CompleteUserUpdateLocked(FrameKey key) {
        FrameRuntimeRecord* record = ResolveReservationLocked(key);
        if (record == nullptr || record->HasUpdateTask) {
            return false;
        }
        const NController::FrameHandle frame = record->Frame;
        try {
            m_frameScheduler->ArmFrame(frame);
            record->UpdateTask = m_taskSystem->Submit([this, frame](NCommon::TaskContext&) { RunUpdate(frame); });
            record->HasUpdateTask = true;
        } catch (...) {
            SetLastErrorLocked(MakeRuntimeError(std::current_exception()));
            m_state = EEngineState::STOPPING;
            m_frameScheduler->AbortFrame(frame);
            *record = {};
            NotifyProgressLocked();
            throw;
        }
        NotifyProgressLocked();
        return true;
    }

    static int ToInt(EEngineState state) noexcept {
        return static_cast<int>(state);
    }

    void RequireRunningLocked(const char* action) const {
        if (m_state != EEngineState::RUNNING) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                                  "Engine cannot {} from state {}",
                                  action,
                                  ToInt(m_state));
        }
    }

    void RollbackStartLocked() noexcept {
        try {
            ClearFrameRecordsLocked();
            m_frameScheduler.reset();
            m_frameRecords.reset();
            m_taskSystem.reset();
        } catch (...) {
        }

        m_state = EEngineState::STOPPED;
        m_updateAdmissionOpen = true;
        m_stopInProgress = false;
        m_activeStopGeneration = m_lifecycleGeneration;
        m_completedStopGeneration = m_lifecycleGeneration;
        m_stopCondition.notify_all();
    }

    void RunUpdate(NController::FrameHandle frame) {
        try {
            NController::FrameScheduler* frameScheduler = nullptr;
            NRuntime::IFrameRuntime* frameRuntime = nullptr;

            {
                std::lock_guard lock{m_mutex};

                if (m_state != EEngineState::RUNNING && m_state != EEngineState::STOPPING) {
                    return;
                }

                frameScheduler = m_frameScheduler.get();
                frameRuntime = m_frameRuntime.get();
            }

            frameScheduler->BeginUpdate(frame);

            const NRuntime::FrameContext context{frame, frameScheduler->GetFrameStorage(frame)};
            frameRuntime->Update(context);

            frameScheduler->EndUpdate(frame);
        } catch (...) {
            LatchRuntimeFailure(frame, std::current_exception());
            throw;
        }
    }

    void RunDraw(NController::FrameHandle frame, NCommon::TaskContext& task) {
        try {
            NController::FrameScheduler* frameScheduler = nullptr;
            NRuntime::IFrameRuntime* frameRuntime = nullptr;

            {
                std::lock_guard lock{m_mutex};

                if (m_state != EEngineState::RUNNING && m_state != EEngineState::STOPPING) {
                    return;
                }

                frameScheduler = m_frameScheduler.get();
                frameRuntime = m_frameRuntime.get();
            }

            frameScheduler->BeginFinalize(frame);

            const NRuntime::FrameContext context{frame, frameScheduler->GetFrameStorage(frame)};
            const auto extraction = frameRuntime->ScheduleExtract(context, task);
            if (extraction.IsValid()) {
                const NCommon::TaskHandle dependencies[] = {extraction};
                const auto finish = task.Spawn(
                        [this, frame, frameScheduler, frameRuntime](NCommon::TaskContext& drawTask) {
                            try {
                                FinishDraw(frame, *frameScheduler, *frameRuntime, drawTask);
                            } catch (...) {
                                LatchRuntimeFailure(frame, std::current_exception());
                                throw;
                            }
                        },
                        dependencies);
                std::lock_guard lock{m_mutex};
                GetFrameRecordLocked(frame).AsyncTask = extraction;
                GetFrameRecordLocked(frame).DrawTask = finish;
                return;
            }

            FinishDraw(frame, *frameScheduler, *frameRuntime, task);
        } catch (...) {
            LatchRuntimeFailure(frame, std::current_exception());
            throw;
        }
    }

    void FinishDraw(NController::FrameHandle frame,
                    NController::FrameScheduler& frameScheduler,
                    NRuntime::IFrameRuntime& frameRuntime,
                    NCommon::TaskContext& task) {
        const NRuntime::FrameContext context{frame, frameScheduler.GetFrameStorage(frame)};

        {
            std::lock_guard lock{m_mutex};
            m_updateAdmissionOpen = true;
            NotifyProgressLocked();
        }

        const auto preparation = frameRuntime.ScheduleDraw(context, task);
        if (preparation.IsValid()) {
            const NCommon::TaskHandle dependencies[] = {preparation};
            const auto finish = task.Spawn(
                    [this, frame, scheduler = &frameScheduler](NCommon::TaskContext&) {
                        try {
                            CompleteDraw(frame, *scheduler);
                        } catch (...) {
                            LatchRuntimeFailure(frame, std::current_exception());
                            throw;
                        }
                    },
                    dependencies);
            std::lock_guard lock{m_mutex};
            GetFrameRecordLocked(frame).AsyncTask = preparation;
            GetFrameRecordLocked(frame).DrawTask = finish;
            return;
        }
        CompleteDraw(frame, frameScheduler);
    }

    void CompleteDraw(NController::FrameHandle frame, NController::FrameScheduler& frameScheduler) {
        if (!frameScheduler.GetGpuCompletion(frame).valid()) {
            frameScheduler.CompleteFrame(frame);
        }
        std::lock_guard lock{m_mutex};
        NotifyProgressLocked();
    }

    void LatchRuntimeFailure(NController::FrameHandle frame, const std::exception_ptr& error) noexcept {
        try {
            std::lock_guard lock{m_mutex};

            SetLastErrorLocked(MakeRuntimeError(error));
            m_state = EEngineState::STOPPING;
            if (!m_frameScheduler->GetGpuCompletion(frame).valid()) {
                ClearFrameRecordLocked(frame);
                m_frameScheduler->AbortFrame(frame);
            }
        } catch (...) {
            SetLastError(MakeRuntimeError(std::current_exception()));
        }
    }

    void SetLastError(NCommon::ErrorInfo error) noexcept {
        try {
            std::lock_guard lock{m_mutex};
            SetLastErrorLocked(std::move(error));
        } catch (...) {
        }
    }

    void SetLastErrorLocked(NCommon::ErrorInfo error) {
        m_lastError = std::move(error);
        NotifyProgressLocked();
    }

    [[nodiscard]] FrameRuntimeRecord& GetFrameRecordLocked(NController::FrameHandle frame) {
        return m_frameRecords[frame.GetSlotIndex()];
    }

    void ClearFrameRecordLocked(NController::FrameHandle frame) {
        FrameRuntimeRecord& record = GetFrameRecordLocked(frame);

        if (record.HasUpdateTask && record.Frame == frame) {
            record = {};
        }
    }

    void ReapTerminalFrameRecordsLocked(NCommon::TaskSystem& taskSystem) {
        if (m_frameRecords == nullptr || m_frameScheduler == nullptr) {
            return;
        }

        for (std::size_t index = 0; index < m_frameScheduler->GetMaxActiveFrames(); ++index) {
            FrameRuntimeRecord& record = m_frameRecords[index];

            if (!record.HasDrawTask) {
                continue;
            }

            const NCommon::ETaskStatus status = taskSystem.GetStatus(record.DrawTask);

            if (!IsTerminalTaskStatus(status)) {
                continue;
            }

            bool failed = status != NCommon::ETaskStatus::COMPLETED;
            if (failed) {
                if (!m_lastError.has_value()) {
                    const auto asyncError =
                            record.AsyncTask.IsValid() ? taskSystem.GetError(record.AsyncTask) : std::nullopt;
                    SetLastErrorLocked(asyncError.value_or(taskSystem.GetError(record.DrawTask)
                                                                   .value_or(MakeRuntimeError(std::make_exception_ptr(
                                                                           std::runtime_error{"Frame task failed"})))));
                }
                m_state = EEngineState::STOPPING;
            }
            const auto completion = m_frameScheduler->GetGpuCompletion(record.Frame);
            if (completion.valid()) {
                if (m_frameScheduler->GetState(record.Frame) == NController::EFrameState::SUBMITTED) {
                    m_frameScheduler->BeginGpu(record.Frame);
                }
                if (completion.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
                    continue;
                }
                try {
                    completion.get();
                } catch (...) {
                    SetLastErrorLocked(MakeRuntimeError(std::current_exception()));
                    m_state = EEngineState::STOPPING;
                    failed = true;
                }
                m_frameScheduler->CompleteGpu(record.Frame);
            }

            if (!failed) {
                m_frameScheduler->RecycleFrame(record.Frame);
                if (completion.valid()) {
                    ++m_gpuRecycledFrameCount;
                }
            } else {
                try {
                    m_frameScheduler->AbortFrame(record.Frame);
                } catch (...) {
                }
            }

            record = {};
        }
    }

    void AbortUnresolvedFrameRecordsLocked() noexcept {
        if (m_frameRecords == nullptr || m_frameScheduler == nullptr) {
            return;
        }

        for (std::size_t index = 0; index < m_frameScheduler->GetMaxActiveFrames(); ++index) {
            FrameRuntimeRecord& record = m_frameRecords[index];

            if (!record.HasReservation) {
                continue;
            }

            try {
                m_frameScheduler->AbortFrame(record.Frame);
            } catch (...) {
            }

            record = {};
        }
    }

    void ClearFrameRecordsLocked() noexcept {
        if (m_frameRecords == nullptr || m_frameScheduler == nullptr) {
            return;
        }

        for (std::size_t index = 0; index < m_frameScheduler->GetMaxActiveFrames(); ++index) {
            m_frameRecords[index] = {};
        }
    }

private:
    const std::uint64_t m_ownerIdentity = AcquireEngineIdentity();
    const EngineConfig m_config;
    std::unique_ptr<NRuntime::IFrameRuntime> m_frameRuntime;

    mutable std::mutex m_mutex;
    std::condition_variable m_stopCondition;
    mutable std::condition_variable m_progressCondition;
    std::uint64_t m_progressVersion = 0;
    EEngineState m_state = EEngineState::CREATED;
    bool m_stopInProgress = false;
    std::uint64_t m_lifecycleGeneration = 0;
    std::atomic<std::uint64_t> m_lifecycleGenerationSnapshot = 0;
    std::uint64_t m_activeStopGeneration = 0;
    std::uint64_t m_completedStopGeneration = 0;
    std::optional<NCommon::ErrorInfo> m_lastError;

    std::unique_ptr<NCommon::TaskSystem> m_taskSystem;
    std::unique_ptr<NController::FrameScheduler> m_frameScheduler;
    std::unique_ptr<FrameRuntimeRecord[]> m_frameRecords;
    std::uint64_t m_nextDrawFrameIndex = 0;
    bool m_updateAdmissionOpen = true;
    std::uint64_t m_gpuRecycledFrameCount = 0;
};

Engine::Engine(EngineConfig config)
    : m_impl(std::make_unique<Impl>(config, NRuntime::MakeDefaultFrameRuntime())) {
}

Engine::Engine(std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl)) {
}

Engine::~Engine() = default;

void Engine::Start() {
    m_impl->Start();
}

void Engine::SubmitUserJob(std::function<void()> function) {
    m_impl->SubmitUserJob(std::move(function));
}

void Engine::Stop() noexcept {
    m_impl->Stop();
}

std::optional<FrameKey> Engine::TryBeginFrame() {
    return m_impl->TryBeginFrame();
}
bool Engine::CompleteUserUpdate(FrameKey frame) {
    return m_impl->CompleteUserUpdate(frame);
}
bool Engine::AbortFrame(FrameKey frame) {
    return m_impl->AbortFrame(frame);
}
std::uint64_t Engine::GetProgressVersion() const {
    return m_impl->GetProgressVersion();
}
void Engine::WaitForProgress(std::uint64_t version, std::chrono::milliseconds timeout) const {
    m_impl->WaitForProgress(version, timeout);
}

bool Engine::Update() {
    return m_impl->Update();
}

bool Engine::Draw() {
    return m_impl->Draw();
}

bool Engine::CanAcceptUpdate() const {
    return m_impl->CanAcceptUpdate();
}

std::uint64_t Engine::GetGpuRecycledFrameCount() const {
    return m_impl->GetGpuRecycledFrameCount();
}

EEngineState Engine::GetState() const noexcept {
    return m_impl->GetState();
}

std::optional<NCommon::ErrorInfo> Engine::GetLastError() const {
    return m_impl->GetLastError();
}

void Engine::ClearLastError() {
    m_impl->ClearLastError();
}

namespace NRuntime {

std::unique_ptr<Engine> EngineFactory::Create(EngineConfig config, std::unique_ptr<IFrameRuntime> frameRuntime) {
    return std::unique_ptr<Engine>{new Engine{std::make_unique<Engine::Impl>(config, std::move(frameRuntime))}};
}

} // namespace NRuntime

} // namespace NEngine
