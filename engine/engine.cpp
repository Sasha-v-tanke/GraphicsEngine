#include "engine.h"

#include <atomic>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
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
            m_taskSystem = std::make_unique<NCommon::TaskSystem>(m_config.WorkerCount);
            m_frameScheduler = std::make_unique<NController::FrameScheduler>(m_config);
            m_frameRecords = std::make_unique<FrameRuntimeRecord[]>(m_frameScheduler->GetMaxActiveFrames());
            m_nextDrawFrameIndex = 0;
            ++m_lifecycleGeneration;
            m_lifecycleGenerationSnapshot.store(m_lifecycleGeneration, std::memory_order_release);
            m_state = EEngineState::RUNNING;
        } catch (...) {
            RollbackStartLocked();
            throw;
        }
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
            taskSystem = std::move(m_taskSystem);
        }

        try {
            if (taskSystem != nullptr) {
                taskSystem->WaitIdle();
            }
        } catch (...) {
            SetLastError(MakeRuntimeError(std::current_exception()));
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
            m_stopInProgress = false;
            m_completedStopGeneration = m_activeStopGeneration;
            m_stopCondition.notify_all();
        }
    }

    bool Update() {
        NController::FrameHandle frame;
        std::lock_guard lock{m_mutex};
        RequireRunningLocked("update");
        ReapTerminalFrameRecordsLocked(*m_taskSystem);

        std::optional<NController::FrameHandle> acquiredFrame = m_frameScheduler->TryAcquireFrame();

        if (!acquiredFrame.has_value()) {
            return false;
        }

        frame = *acquiredFrame;
        m_frameScheduler->ArmFrame(frame);

        NCommon::TaskHandle updateTask;

        try {
            updateTask = m_taskSystem->Submit([this, frame](NCommon::TaskContext&) { RunUpdate(frame); });
        } catch (...) {
            SetLastErrorLocked(MakeRuntimeError(std::current_exception()));
            m_state = EEngineState::STOPPING;
            m_frameScheduler->AbortFrame(frame);
            throw;
        }

        FrameRuntimeRecord& record = GetFrameRecordLocked(frame);
        record.Frame = frame;
        record.UpdateTask = std::move(updateTask);
        record.HasUpdateTask = true;

        return true;
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
            const NCommon::TaskHandle dependencies[] = {updateTask};

            record.DrawTask =
                    m_taskSystem->Submit([this, frame](NCommon::TaskContext&) { RunDraw(frame); }, dependencies);
            record.HasDrawTask = true;
            m_frameScheduler->SignalDraw(frame);
        } catch (...) {
            SetLastErrorLocked(MakeRuntimeError(std::current_exception()));
            throw;
        }

        ++m_nextDrawFrameIndex;

        return true;
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
        NCommon::TaskHandle UpdateTask;
        NCommon::TaskHandle DrawTask;
        bool HasUpdateTask = false;
        bool HasDrawTask = false;
    };

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

            const NRuntime::FrameContext context{frame};
            frameRuntime->Update(context);

            frameScheduler->EndUpdate(frame);
        } catch (...) {
            LatchRuntimeFailure(frame, std::current_exception());
            throw;
        }
    }

    void RunDraw(NController::FrameHandle frame) {
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

            const NRuntime::FrameContext context{frame};
            frameRuntime->Draw(context);

            frameScheduler->CompleteFrame(frame);
        } catch (...) {
            LatchRuntimeFailure(frame, std::current_exception());
            throw;
        }
    }

    void LatchRuntimeFailure(NController::FrameHandle frame, const std::exception_ptr& error) noexcept {
        try {
            std::lock_guard lock{m_mutex};

            SetLastErrorLocked(MakeRuntimeError(error));
            m_state = EEngineState::STOPPING;
            ClearFrameRecordLocked(frame);
            m_frameScheduler->AbortFrame(frame);
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

            if (status == NCommon::ETaskStatus::COMPLETED) {
                m_frameScheduler->RecycleFrame(record.Frame);
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

            if (!record.HasUpdateTask) {
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
    const EngineConfig m_config;
    std::unique_ptr<NRuntime::IFrameRuntime> m_frameRuntime;

    mutable std::mutex m_mutex;
    std::condition_variable m_stopCondition;
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

void Engine::Stop() noexcept {
    m_impl->Stop();
}

bool Engine::Update() {
    return m_impl->Update();
}

bool Engine::Draw() {
    return m_impl->Draw();
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
