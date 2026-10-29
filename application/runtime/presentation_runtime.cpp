#include "presentation_runtime.h"

#include <chrono>
#include <exception>
#include <stdexcept>
#include <thread>
#include <utility>

#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NApplication::NRuntime {

PresentationRuntime::PresentationRuntime(std::unique_ptr<NGraphics::Graphics> graphics)
    : m_graphics(std::move(graphics)) {
    if (!m_graphics) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Presentation Graphics is null");
    }
}

void PresentationRuntime::Start() {
    std::lock_guard lock{m_mutex};
    if (m_failure) {
        std::rethrow_exception(m_failure);
    }
    m_stopping = false;
    m_nextFrameIndex = 0;
    m_diagnostics = {};
}

void PresentationRuntime::Draw(const NEngine::NRuntime::FrameContext& frame) {
    auto pending = std::make_unique<PendingFrame>();
    pending->Submission.FrameIndex = frame.GetFrame().GetFrameIndex();
    pending->Submission.FrameSlotIndex = frame.GetFrame().GetSlotIndex();
    const auto future = pending->Promise.get_future().share();
    std::lock_guard lock{m_mutex};
    const auto [it, inserted] = m_queued.emplace(pending->Submission.FrameIndex, std::move(pending));
    if (!inserted) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Duplicate presentation frame");
    }
    // Before publication to Pump: an error after native submit cannot make Engine recycle the slot.
    try {
        frame.RegisterGpuCompletion(future);
    } catch (...) {
        m_queued.erase(it);
        throw;
    }
}

void PresentationRuntime::CompletePending() {
    while (!m_submitted.empty()) {
        PendingFrame& frame = *m_submitted.front();
        if (!m_graphics->IsCompleted(frame.Completion)) {
            return;
        }
        if (frame.Failure) {
            frame.Promise.set_exception(frame.Failure);
        } else {
            frame.Promise.set_value();
        }
        ++m_diagnostics.Completed;
        m_submitted.pop_front();
    }
}

void PresentationRuntime::Pump(int width, int height) {
    std::lock_guard lock{m_mutex};
    try {
        CompletePending();
        if (m_stopping) {
            return;
        }
        m_graphics->SetPresentationSize(width, height);
        const auto it = m_queued.find(m_nextFrameIndex);
        if (it == m_queued.end()) {
            return;
        }
        // Reserve the destination before submit; no allocation is allowed to lose an accepted completion.
        m_submitted.push_back(nullptr);
        NGraphics::FrameSubmissionResult result;
        try {
            result = m_graphics->TrySubmitFrame(it->second->Submission);
        } catch (...) {
            m_submitted.pop_back();
            throw;
        }
        if (!result.Completion.IsValid()) {
            m_submitted.pop_back();
            if (result.Failure) {
                std::rethrow_exception(result.Failure);
            }
            return;
        }
        it->second->Completion = result.Completion;
        it->second->Failure = result.Failure;
        m_submitted.back() = std::move(it->second);
        m_queued.erase(it);
        ++m_nextFrameIndex;
        ++m_diagnostics.Submitted;
        if (result.Failure) {
            std::rethrow_exception(result.Failure);
        }
    } catch (...) {
        m_stopping = true;
        m_failure = std::current_exception();
        // Submitted promises remain pending until actual GPU completion (or fatal device failure in Drain).
        throw;
    }
}

FrameDiagnostics PresentationRuntime::GetDiagnostics() {
    std::lock_guard lock{m_mutex};
    return m_diagnostics;
}

void PresentationRuntime::FailAll(const std::exception_ptr& error) noexcept {
    for (auto& [_, frame]: m_queued) {
        frame->Promise.set_exception(error);
    }
    for (auto& frame: m_submitted) {
        frame->Promise.set_exception(error);
    }
    m_queued.clear();
    m_submitted.clear();
}

void PresentationRuntime::Drain() noexcept {
    std::lock_guard lock{m_mutex};
    m_stopping = true;
    try {
        // No native submit for queued intents after admission closes. Their CPU producers have joined.
        for (auto& [_, frame]: m_queued) {
            frame->Promise.set_value();
            ++m_diagnostics.Abandoned;
        }
        m_queued.clear();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        while (!m_submitted.empty()) {
            CompletePending();
            if (std::chrono::steady_clock::now() >= deadline) {
                GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Presentation GPU drain timed out");
            }
            if (!m_submitted.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        }
    } catch (...) {
        m_failure = std::current_exception();
        FailAll(m_failure);
    }
}
} // namespace NApplication::NRuntime
