#include "engine.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

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

} // namespace

namespace NRuntime {

class DefaultFrameRuntime final: public IFrameRuntime {
public:
    void Update(NController::FrameScheduler& frameScheduler, NController::FrameHandle frame) override {
        frameScheduler.BeginUpdate(frame);
        frameScheduler.EndUpdate(frame);
    }

    void Draw(NController::FrameScheduler& frameScheduler, NController::FrameHandle frame) override {
        frameScheduler.BeginFinalize(frame);
        frameScheduler.CompleteFrame(frame);
        frameScheduler.RecycleFrame(frame);
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
        m_state = EEngineState::RUNNING;

        try {
            m_taskSystem = std::make_unique<NCommon::TaskSystem>(m_config.WorkerCount);
            m_frameScheduler = std::make_unique<NController::FrameScheduler>(m_config);
        } catch (...) {
            RollbackStartLocked();
            throw;
        }
    }

    void Stop() noexcept {
        std::unique_ptr<NCommon::TaskSystem> taskSystem;

        {
            std::lock_guard lock{m_mutex};

            if (m_state == EEngineState::CREATED || m_state == EEngineState::STOPPED) {
                m_state = EEngineState::STOPPED;
                return;
            }

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

            m_pendingFrames.clear();
            m_frameScheduler.reset();
            taskSystem.reset();
            m_state = EEngineState::STOPPED;
        }
    }

    bool Update() {
        NController::FrameHandle frame;
        std::lock_guard lock{m_mutex};
        RequireRunningLocked("update");

        std::optional<NController::FrameHandle> acquiredFrame = m_frameScheduler->TryAcquireFrame();

        if (!acquiredFrame.has_value()) {
            return false;
        }

        frame = *acquiredFrame;
        m_frameScheduler->ArmFrame(frame);

        NCommon::TaskHandle updateTask;

        try {
            m_pendingFrames.reserve(m_pendingFrames.size() + 1);
            updateTask = m_taskSystem->Submit([this, frame](NCommon::TaskContext&) { RunUpdate(frame); });
        } catch (...) {
            SetLastErrorLocked(MakeRuntimeError(std::current_exception()));
            m_frameScheduler->AbortFrame(frame);
            throw;
        }

        m_pendingFrames.push_back(PendingFrame{
                .Frame = frame,
                .UpdateTask = std::move(updateTask),
        });

        return true;
    }

    bool Draw() {
        std::lock_guard lock{m_mutex};
        RequireRunningLocked("draw");

        if (m_pendingFrames.empty()) {
            return false;
        }

        const PendingFrame pendingFrame = m_pendingFrames.front();

        try {
            const NCommon::TaskHandle dependencies[] = {pendingFrame.UpdateTask};

            static_cast<void>(
                    m_taskSystem->Submit([this, frame = pendingFrame.Frame](NCommon::TaskContext&) { RunDraw(frame); },
                                         dependencies));
        } catch (...) {
            SetLastErrorLocked(MakeRuntimeError(std::current_exception()));
            throw;
        }

        m_pendingFrames.erase(m_pendingFrames.begin());

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
    struct PendingFrame {
        NController::FrameHandle Frame;
        NCommon::TaskHandle UpdateTask;
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
            m_pendingFrames.clear();
            m_frameScheduler.reset();
            m_taskSystem.reset();
        } catch (...) {
        }

        m_state = EEngineState::STOPPED;
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

            frameRuntime->Update(*frameScheduler, frame);
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

            frameRuntime->Draw(*frameScheduler, frame);
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
            RemovePendingFrameLocked(frame);
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

    void RemovePendingFrameLocked(NController::FrameHandle frame) {
        const auto it = std::ranges::find_if(m_pendingFrames, [frame](const PendingFrame& pendingFrame) {
            return pendingFrame.Frame == frame;
        });

        if (it != m_pendingFrames.end()) {
            m_pendingFrames.erase(it);
        }
    }

private:
    const EngineConfig m_config;
    std::unique_ptr<NRuntime::IFrameRuntime> m_frameRuntime;

    mutable std::mutex m_mutex;
    EEngineState m_state = EEngineState::CREATED;
    std::optional<NCommon::ErrorInfo> m_lastError;

    std::unique_ptr<NCommon::TaskSystem> m_taskSystem;
    std::unique_ptr<NController::FrameScheduler> m_frameScheduler;
    std::vector<PendingFrame> m_pendingFrames;
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
