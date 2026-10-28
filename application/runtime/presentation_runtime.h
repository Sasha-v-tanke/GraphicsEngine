#pragma once

#include <cstdint>
#include <deque>
#include <future>
#include <map>
#include <memory>
#include <mutex>

#include <application/frame_diagnostics.h>
#include <engine/runtime/frame_runtime.h>
#include <graphics/graphics.h>

namespace NApplication::NRuntime {

// Draw publishes bounded frame intents from workers. Only Pump/Drain touch Graphics, on the application thread.
class PresentationRuntime final: public NEngine::NRuntime::IFrameRuntime {
public:
    explicit PresentationRuntime(std::unique_ptr<NGraphics::Graphics> graphics);
    void Update(const NEngine::NRuntime::FrameContext&) override {
    }
    void Draw(const NEngine::NRuntime::FrameContext& frame) override;
    void Pump(int width, int height);
    void Drain() noexcept override;
    void Start();
    [[nodiscard]] FrameDiagnostics GetDiagnostics();

private:
    struct PendingFrame {
        NGraphics::FrameSubmission Submission;
        std::promise<void> Promise;
        NGraphics::CompletionPoint Completion;
        std::exception_ptr Failure = {};
    };
    void CompletePending();
    void FailAll(std::exception_ptr error) noexcept;

    std::unique_ptr<NGraphics::Graphics> m_graphics;
    std::mutex m_mutex;
    std::map<std::uint64_t, std::unique_ptr<PendingFrame>> m_queued;
    std::deque<std::unique_ptr<PendingFrame>> m_submitted;
    std::uint64_t m_nextFrameIndex = 0;
    bool m_stopping = false;
    FrameDiagnostics m_diagnostics;
    std::exception_ptr m_failure;
};
} // namespace NApplication::NRuntime
