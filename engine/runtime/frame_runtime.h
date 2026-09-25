#pragma once

#include <memory>
#include <memory_resource>

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

private:
    NController::FrameHandle m_frame;
    NController::FrameStorage m_storage;
};

class IFrameRuntime {
public:
    virtual ~IFrameRuntime() = default;

    virtual void Update(const FrameContext& frame) = 0;

    virtual void Draw(const FrameContext& frame) = 0;
};

[[nodiscard]] std::unique_ptr<IFrameRuntime> MakeDefaultFrameRuntime();

class EngineFactory final {
public:
    [[nodiscard]] static std::unique_ptr<Engine> Create(EngineConfig config,
                                                        std::unique_ptr<IFrameRuntime> frameRuntime);
};

} // namespace NEngine::NRuntime
