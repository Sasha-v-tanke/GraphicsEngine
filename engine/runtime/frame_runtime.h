#pragma once

#include <future>
#include <memory>
#include <memory_resource>

#include <GraphicsEngine/lib/thread/task/task_system.h>
#include <engine/controller/frame_scheduler.h>
#include <engine/engine.h>
#include <engine/engine_config.h>

namespace NEngine::NRuntime {

class FrameContext final {
public:
    FrameContext(NController::FrameHandle frame, NController::FrameStorage storage) noexcept
        : m_frame(frame)
        , m_storage(storage) {
    }

    [[nodiscard]] NController::FrameHandle GetFrame() const noexcept {
        return m_frame;
    }

    [[nodiscard]] NController::FrameStorage GetStorage() const noexcept {
        return m_storage;
    }

    [[nodiscard]] std::pmr::memory_resource& GetMemoryResource() const {
        return m_storage.GetMemoryResource();
    }

    void RegisterGpuCompletion(std::shared_future<void> completion) const {
        m_storage.RegisterGpuCompletion(std::move(completion));
    }

private:
    NController::FrameHandle m_frame;
    NController::FrameStorage m_storage;
};

class IFrameRuntime {
public:
    virtual ~IFrameRuntime() = default;

    virtual void Update(const FrameContext& frame) = 0;

    virtual void Extract(const FrameContext&) {
    }

    // Scheduled work must depend on task.GetTask(). Its join completes only after all World readers finish,
    // including failure paths. The engine keeps update admission closed until that join completes.
    // An invalid handle means extraction completed synchronously. Do not cancel a join with live World readers.
    [[nodiscard]] virtual NCommon::TaskHandle ScheduleExtract(const FrameContext& frame, NCommon::TaskContext&) {
        Extract(frame);
        return {};
    }

    virtual void Draw(const FrameContext& frame) = 0;

    // Called by Stop on the caller after CPU producers finish, before waiting for registered completions.
    virtual void Drain() noexcept {
    }

    // Same gating/draining contract as ScheduleExtract; frame storage stays live through the returned join.
    [[nodiscard]] virtual NCommon::TaskHandle ScheduleDraw(const FrameContext& frame, NCommon::TaskContext&) {
        Draw(frame);
        return {};
    }
};

[[nodiscard]] std::unique_ptr<IFrameRuntime> MakeDefaultFrameRuntime();

class EngineFactory final {
public:
    [[nodiscard]] static std::unique_ptr<Engine> Create(EngineConfig config,
                                                        std::unique_ptr<IFrameRuntime> frameRuntime);
};

} // namespace NEngine::NRuntime
