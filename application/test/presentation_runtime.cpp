#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <vector>

#include <application/runtime/presentation_runtime.h>
#include <engine/controller/frame_scheduler.h>
#include <graphics/backend/backend.h>
#include <graphics/backend/factory.h>
#include <gtest/gtest.h>

namespace {
struct WorkProbe {
    using Clock = std::chrono::steady_clock;

    WorkProbe()
        : Finished(FinishedSignal.get_future()) {
    }

    static std::uint64_t RunWork(std::atomic<std::uint64_t>& progress) {
        const auto deadline = Clock::now() + std::chrono::milliseconds{40};
        std::uint64_t checksum = 1;
        do {
            for (std::size_t index = 0; index < 1024; ++index) {
                checksum = checksum * 1664525U + static_cast<std::uint64_t>(index);
            }
            progress.fetch_add(1024, std::memory_order_relaxed);
        } while (Clock::now() < deadline);
        return checksum;
    }

    void Release() noexcept {
        if (!Released.exchange(true)) {
            Go.release();
        }
    }

    void RunUpdate() {
        Ready.store(true, std::memory_order_release);
        Go.acquire();
        UpdateStart = Clock::now();
        Started.store(true, std::memory_order_release);
        UpdateChecksum = RunWork(UpdateProgress);
        UpdateEnd = Clock::now();
        FinishedSignal.set_value();
    }

    void RunBackend() {
        const auto timeout = Clock::now() + std::chrono::seconds{2};
        while (!Ready.load(std::memory_order_acquire) && Clock::now() < timeout) {
            std::this_thread::yield();
        }
        Release();
        while (!Started.load(std::memory_order_acquire) && Clock::now() < timeout) {
            std::this_thread::yield();
        }
        BackendStart = Clock::now();
        UpdateProgressAtBackendStart = UpdateProgress.load(std::memory_order_relaxed);
        BackendChecksum = RunWork(BackendProgress);
        UpdateProgressAtBackendEnd = UpdateProgress.load(std::memory_order_relaxed);
        BackendEnd = Clock::now();
    }

    std::binary_semaphore Go{0};
    std::atomic_bool Released{false};
    std::atomic_bool Ready{false};
    std::atomic_bool Started{false};
    std::atomic<std::uint64_t> UpdateProgress{0};
    std::atomic<std::uint64_t> BackendProgress{0};
    std::promise<void> FinishedSignal;
    std::future<void> Finished;
    Clock::time_point UpdateStart;
    Clock::time_point UpdateEnd;
    Clock::time_point BackendStart;
    Clock::time_point BackendEnd;
    std::uint64_t UpdateProgressAtBackendStart = 0;
    std::uint64_t UpdateProgressAtBackendEnd = 0;
    std::uint64_t UpdateChecksum = 0;
    std::uint64_t BackendChecksum = 0;
};

class ScriptedBackend final: public NGraphics::NBackend::IGraphicsBackend {
public:
    NGraphics::GraphicsCapabilities Capabilities{.Presentation = true,
                                                 .TimelineCompletion = true,
                                                 .MaxFramesInFlight = 3};
    std::vector<NGraphics::FrameSubmission> Attempts;
    std::uint64_t CompletedValue = 0;
    std::uint64_t NextValue = 0;
    int NotReadyCount = 0;
    int OutOfDateCount = 0;
    bool FailBeforeSubmit = false;
    bool FailAfterSubmit = false;
    bool DeviceLost = false;
    bool Suspended = false;
    WorkProbe* ActiveWork = nullptr;
    [[nodiscard]] const NGraphics::GraphicsCapabilities& GetCapabilities() const noexcept override {
        return Capabilities;
    }
    std::uint64_t SubmitFrame(NGraphics::FrameSubmission) override {
        throw std::runtime_error{"wrong API"};
    }
    void SetPresentationSize(int width, int height) override {
        Suspended = width <= 0 || height <= 0;
    }
    NGraphics::NBackend::BackendFrameResult TrySubmitFrame(NGraphics::FrameSubmission submission) override {
        Attempts.push_back(submission);
        if (ActiveWork != nullptr) {
            ActiveWork->RunBackend();
        }
        if (FailBeforeSubmit) {
            return {.Status = NGraphics::EFrameSubmissionStatus::FAILED,
                    .Failure = std::make_exception_ptr(std::runtime_error{"record/submit failed"})};
        }
        if (Suspended) {
            return {.Status = NGraphics::EFrameSubmissionStatus::SUSPENDED};
        }
        if (NotReadyCount > 0) {
            --NotReadyCount;
            return {.Status = NGraphics::EFrameSubmissionStatus::NOT_READY};
        }
        if (OutOfDateCount > 0) {
            --OutOfDateCount;
            return {.Status = NGraphics::EFrameSubmissionStatus::OUT_OF_DATE};
        }
        return {.Status = FailAfterSubmit ? NGraphics::EFrameSubmissionStatus::FAILED
                                          : NGraphics::EFrameSubmissionStatus::PRESENTED,
                .CompletionValue = ++NextValue,
                .Failure = FailAfterSubmit ? std::make_exception_ptr(std::runtime_error{"present failed"}) : nullptr};
    }
    [[nodiscard]] bool IsCompleted(std::uint64_t value) const override {
        if (DeviceLost) {
            throw std::runtime_error{"device lost"};
        }
        return value <= CompletedValue;
    }
    std::uint64_t CreateBuffer(const NGraphics::BufferDescriptor&) override {
        return 1;
    }
    void DestroyBuffer(std::uint64_t, std::optional<std::uint64_t>) noexcept override {
    }
    std::uint64_t CreateGraphicsPipeline(const NGraphics::GraphicsPipelineDescriptor&) override {
        return 1;
    }
    void DestroyGraphicsPipeline(std::uint64_t, std::optional<std::uint64_t>) noexcept override {
    }
    std::uint64_t CreateImage(const NGraphics::ImageDescriptor&) override {
        return 1;
    }
    void DestroyImage(std::uint64_t, std::optional<std::uint64_t>) noexcept override {
    }
    std::uint64_t CreateImageView(const NGraphics::ImageViewDescriptor&) override {
        return 1;
    }
    void DestroyImageView(std::uint64_t, std::optional<std::uint64_t>) noexcept override {
    }
    std::uint64_t CreateSampler(const NGraphics::SamplerDescriptor&) override {
        return 1;
    }
    void DestroySampler(std::uint64_t, std::optional<std::uint64_t>) noexcept override {
    }
};

struct Fixture {
    ScriptedBackend* Backend = nullptr;
    std::unique_ptr<NApplication::NRuntime::PresentationRuntime> Runtime;
    NEngine::NController::FrameScheduler Scheduler;
    explicit Fixture(std::size_t count = 1)
        : Scheduler({.MaxActiveFrames = count}) {
        auto backend = std::make_unique<ScriptedBackend>();
        Backend = backend.get();
        // Graphics is nonmovable: direct new preserves guaranteed copy elision, unlike make_unique.
        // NOLINTNEXTLINE(modernize-make-unique)
        std::unique_ptr<NGraphics::Graphics> graphics{
                new NGraphics::Graphics{NGraphics::NBackend::CreateGraphicsForBackend(std::move(backend))}};
        Runtime = std::make_unique<NApplication::NRuntime::PresentationRuntime>(std::move(graphics));
        Runtime->Start();
    }
    NEngine::NController::FrameHandle Enqueue() {
        const auto frame = *Scheduler.TryAcquireFrame();
        Scheduler.ArmFrame(frame);
        Scheduler.BeginUpdate(frame);
        Scheduler.EndUpdate(frame);
        Scheduler.SignalDraw(frame);
        Scheduler.BeginFinalize(frame);
        Runtime->Draw({frame, Scheduler.GetFrameStorage(frame)});
        return frame;
    }
};

TEST(PresentationRuntime, RetainsFrameAndIdentityAcrossBackpressureAndSuspension) {
    Fixture fixture;
    const auto frame = fixture.Enqueue();
    const auto completion = fixture.Scheduler.GetGpuCompletion(frame);
    fixture.Backend->NotReadyCount = 1;
    fixture.Backend->OutOfDateCount = 1;
    fixture.Runtime->Pump(64, 64);
    EXPECT_EQ(fixture.Backend->NotReadyCount, 0);
    EXPECT_EQ(fixture.Backend->OutOfDateCount, 1);
    EXPECT_EQ(completion.wait_for(std::chrono::seconds{0}), std::future_status::timeout);
    fixture.Runtime->Pump(64, 64);
    EXPECT_EQ(fixture.Backend->OutOfDateCount, 0);
    fixture.Runtime->Pump(0, 0);
    EXPECT_EQ(fixture.Runtime->GetDiagnostics().Submitted, 0);
    EXPECT_EQ(completion.wait_for(std::chrono::seconds{0}), std::future_status::timeout);
    fixture.Runtime->Pump(64, 64);
    EXPECT_EQ(fixture.Runtime->GetDiagnostics().Submitted, 1);
    EXPECT_FALSE(fixture.Scheduler.TryAcquireFrame().has_value());
    for (const auto& submission: fixture.Backend->Attempts) {
        EXPECT_EQ(submission.FrameIndex, frame.GetFrameIndex());
        EXPECT_EQ(submission.FrameSlotIndex, frame.GetSlotIndex());
    }
    fixture.Backend->CompletedValue = 1;
    fixture.Runtime->Pump(64, 64);
    EXPECT_NO_THROW(completion.get());
    fixture.Scheduler.BeginGpu(frame);
    fixture.Scheduler.CompleteGpu(frame);
    fixture.Scheduler.RecycleFrame(frame);
    const auto reused = fixture.Enqueue();
    EXPECT_EQ(reused.GetSlotIndex(), frame.GetSlotIndex());
    EXPECT_EQ(reused.GetFrameIndex(), frame.GetFrameIndex() + 1);
    const auto nextCompletion = fixture.Scheduler.GetGpuCompletion(reused);
    EXPECT_EQ(nextCompletion.wait_for(std::chrono::seconds{0}), std::future_status::timeout);
    fixture.Runtime->Pump(64, 64);
    EXPECT_EQ(fixture.Backend->Attempts.back().FrameIndex, reused.GetFrameIndex());
    EXPECT_EQ(nextCompletion.wait_for(std::chrono::seconds{0}), std::future_status::timeout);
    fixture.Backend->CompletedValue = 2;
    fixture.Runtime->Pump(64, 64);
    EXPECT_NO_THROW(nextCompletion.get());
    fixture.Runtime->Drain();
}

TEST(PresentationRuntime, OutOfOrderWorkersCannotReorderSubmissions) {
    Fixture fixture{3};
    // Build frame contexts, then publish draw intents in reverse worker completion order.
    std::vector<NEngine::NController::FrameHandle> frames;
    for (int index = 0; index < 3; ++index) {
        const auto frame = *fixture.Scheduler.TryAcquireFrame();
        fixture.Scheduler.ArmFrame(frame);
        fixture.Scheduler.BeginUpdate(frame);
        fixture.Scheduler.EndUpdate(frame);
        fixture.Scheduler.SignalDraw(frame);
        fixture.Scheduler.BeginFinalize(frame);
        frames.push_back(frame);
    }
    for (std::size_t index = 3; index-- > 0;) {
        fixture.Runtime->Draw({frames[index], fixture.Scheduler.GetFrameStorage(frames[index])});
        fixture.Runtime->Pump(64, 64);
    }
    fixture.Runtime->Pump(64, 64);
    fixture.Runtime->Pump(64, 64);
    ASSERT_EQ(fixture.Backend->Attempts.size(), 3);
    for (std::size_t index = 0; index < 3; ++index) {
        EXPECT_EQ(fixture.Backend->Attempts[index].FrameIndex, index);
        EXPECT_EQ(fixture.Backend->Attempts[index].FrameSlotIndex, index);
    }
    fixture.Backend->CompletedValue = 3;
    fixture.Runtime->Drain();
    EXPECT_EQ(fixture.Runtime->GetDiagnostics().Completed, 3);
}

TEST(PresentationRuntime, PresentFailureKeepsCompletionPendingUntilGpuFinishes) {
    Fixture fixture;
    const auto frame = fixture.Enqueue();
    const auto completion = fixture.Scheduler.GetGpuCompletion(frame);
    fixture.Backend->FailAfterSubmit = true;
    EXPECT_THROW(fixture.Runtime->Pump(64, 64), std::runtime_error);
    EXPECT_EQ(completion.wait_for(std::chrono::seconds{0}), std::future_status::timeout);
    EXPECT_FALSE(fixture.Scheduler.TryAcquireFrame().has_value());
    fixture.Backend->CompletedValue = 1;
    fixture.Runtime->Drain();
    EXPECT_THROW(completion.get(), std::runtime_error);
    EXPECT_EQ(fixture.Runtime->GetDiagnostics().Submitted, 1);
    EXPECT_EQ(fixture.Runtime->GetDiagnostics().Completed, 1);
}

TEST(PresentationRuntime, DeviceLossTerminatesCompletionWithFailure) {
    Fixture fixture;
    const auto frame = fixture.Enqueue();
    const auto completion = fixture.Scheduler.GetGpuCompletion(frame);
    fixture.Runtime->Pump(64, 64);
    fixture.Backend->DeviceLost = true;
    EXPECT_THROW(fixture.Runtime->Pump(64, 64), std::runtime_error);
    fixture.Runtime->Drain();
    EXPECT_THROW(completion.get(), std::runtime_error);
    EXPECT_EQ(fixture.Runtime->GetDiagnostics().Completed, 0);
}

TEST(PresentationRuntime, RecordFailureDoesNotProduceSubmissionAndShutdownCancelsQueuedIntent) {
    Fixture fixture;
    const auto frame = fixture.Enqueue();
    fixture.Backend->FailBeforeSubmit = true;
    EXPECT_THROW(fixture.Runtime->Pump(64, 64), std::runtime_error);
    fixture.Runtime->Drain();
    EXPECT_EQ(fixture.Runtime->GetDiagnostics().Submitted, 0);
    EXPECT_EQ(fixture.Runtime->GetDiagnostics().Abandoned, 1);
    EXPECT_NO_THROW(fixture.Scheduler.GetGpuCompletion(frame).get());
}

TEST(PresentationRuntime, BackendSubmitOverlapsNextUpdateAndPreviousGpu) {
    auto backend = std::make_unique<ScriptedBackend>();
    auto* native = backend.get();
    std::unique_ptr<NGraphics::Graphics> graphics{
            new NGraphics::Graphics{NGraphics::NBackend::CreateGraphicsForBackend(std::move(backend))}};
    auto runtime = std::make_unique<NApplication::NRuntime::PresentationRuntime>(std::move(graphics));
    auto* presentation = runtime.get();
    presentation->Start();
    auto engine =
            NEngine::NRuntime::EngineFactory::Create({.MaxActiveFrames = 3, .WorkerCount = 2}, std::move(runtime));
    struct ReleaseGuard {
        ScriptedBackend& Backend;
        std::vector<std::shared_ptr<WorkProbe>> Probes;
        ~ReleaseGuard() {
            Backend.ActiveWork = nullptr;
            for (const auto& probe: Probes) {
                probe->Release();
            }
            Backend.CompletedValue = Backend.NextValue;
        }
    } release{.Backend = *native, .Probes = {}};
    engine->Start();

    ASSERT_TRUE(engine->Update());
    ASSERT_TRUE(engine->Draw());
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (presentation->GetDiagnostics().Submitted < 1 && std::chrono::steady_clock::now() < deadline) {
        presentation->Pump(64, 64);
        std::this_thread::yield();
    }
    ASSERT_EQ(presentation->GetDiagnostics().Submitted, 1);
    ASSERT_EQ(presentation->GetDiagnostics().Completed, 0);

    std::optional<NEngine::FrameKey> first;
    while (!first && std::chrono::steady_clock::now() < deadline) {
        first = engine->TryBeginFrame();
        std::this_thread::yield();
    }
    ASSERT_TRUE(first);
    ASSERT_TRUE(engine->CompleteUserUpdate(*first));
    ASSERT_TRUE(engine->Draw());
    constexpr std::uint64_t CYCLES = 8;
    std::vector<std::chrono::microseconds> overlaps;
    for (std::uint64_t frame = 1; frame <= CYCLES; ++frame) {
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        std::optional<NEngine::FrameKey> next;
        while (!next && std::chrono::steady_clock::now() < deadline) {
            next = engine->TryBeginFrame();
            std::this_thread::yield();
        }
        ASSERT_TRUE(next) << "Frame " << frame + 1;
        ASSERT_EQ(next->GetFrameSequence(), frame + 1);
        auto probe = std::make_shared<WorkProbe>();
        release.Probes.push_back(probe);
        ASSERT_TRUE(engine->ScheduleUserUpdate(*next, [probe] { probe->RunUpdate(); }));
        native->ActiveWork = probe.get();
        while (presentation->GetDiagnostics().Submitted < frame + 1 && std::chrono::steady_clock::now() < deadline) {
            presentation->Pump(64, 64);
            std::this_thread::yield();
        }
        native->ActiveWork = nullptr;
        ASSERT_EQ(presentation->GetDiagnostics().Submitted, frame + 1);
        ASSERT_EQ(probe->Finished.wait_for(std::chrono::seconds{2}), std::future_status::ready);
        EXPECT_GT(probe->UpdateProgress.load(), 0);
        EXPECT_GT(probe->BackendProgress.load(), 0);
        EXPECT_NE(probe->UpdateChecksum, 0);
        EXPECT_NE(probe->BackendChecksum, 0);
        EXPECT_GT(probe->UpdateProgressAtBackendEnd, probe->UpdateProgressAtBackendStart);
        const auto intersection =
                std::min(probe->UpdateEnd, probe->BackendEnd) - std::max(probe->UpdateStart, probe->BackendStart);
        EXPECT_GT(intersection, WorkProbe::Clock::duration::zero());
        overlaps.push_back(std::chrono::duration_cast<std::chrono::microseconds>(intersection));
        EXPECT_EQ(presentation->GetDiagnostics().Completed, frame - 1);
        EXPECT_EQ(native->CompletedValue, frame - 1);

        ASSERT_TRUE(engine->Draw());
        EXPECT_FALSE(engine->TryBeginFrame());
        native->CompletedValue = frame;
        presentation->Pump(0, 0);
        EXPECT_EQ(presentation->GetDiagnostics().Completed, frame);
    }
    std::cout << "Frame pipeline overlap (us), GPU N-1 pending:";
    for (std::size_t index = 0; index < overlaps.size(); ++index) {
        std::cout << ' ' << index + 1 << ':' << overlaps[index].count();
    }
    std::cout << '\n';
    native->CompletedValue = native->NextValue;
    engine->Stop();
    EXPECT_FALSE(engine->GetLastError());
}
} // namespace
