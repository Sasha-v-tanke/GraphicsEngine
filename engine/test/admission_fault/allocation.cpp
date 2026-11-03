#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <future>
#include <memory>
#include <new>
#include <semaphore>
#include <utility>

#include <engine/engine.h>
#include <engine/runtime/frame_runtime.h>
#include <gtest/gtest.h>
#include <lib/thread/task/task_system.h>

namespace {

thread_local std::ptrdiff_t AllocationBudget = -1;

} // namespace

// This executable is isolated from the other suites. Fail only one allocation on the submitting
// thread, so the capture's reentrant Submit can allocate normally while unwinding.
// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name): Standard-library names vary by implementation.
void* operator new(std::size_t size) {
    if (AllocationBudget == 0) {
        AllocationBudget = -1;
        throw std::bad_alloc{};
    }
    if (AllocationBudget > 0) {
        --AllocationBudget;
    }
#ifdef GRAPHICS_ENGINE_ADMISSION_FAULT_ASAN
    // ASan's nothrow interceptor allocates with operator-new provenance. Its default delete
    // must release these blocks, including allocations made inside the shared C++ runtime.
    if (void* memory = ::operator new(size, std::nothrow)) {
        return memory;
    }
#else
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
#endif
    throw std::bad_alloc{};
}

#ifndef GRAPHICS_ENGINE_ADMISSION_FAULT_ASAN
// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name): Standard-library names vary by implementation.
void operator delete(void* memory) noexcept {
    std::free(memory);
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name): Standard-library names vary by implementation.
void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name): Standard-library names vary by implementation.
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name): Standard-library names vary by implementation.
void operator delete(void* memory, const std::nothrow_t&) noexcept {
    ::operator delete(memory);
}
#endif

namespace {
struct ReentrantCapture {
    NEngine::Engine* Engine;
    std::atomic<int>* Destroyed;
    ~ReentrantCapture() {
        static_cast<void>(Engine->GetState());
        ++*Destroyed;
    }
};
std::function<void()> MakeCallback(NEngine::Engine& engine, std::atomic<int>& destroyed) {
    auto capture = std::make_shared<ReentrantCapture>(&engine, &destroyed);
    // A heap closure transfers its last capture owner on both libc++ and libstdc++ std::function moves.
    return [capture = std::move(capture), padding = std::array<std::byte, 64>{}] { static_cast<void>(padding); };
}

TEST(EngineAdmission, RejectedCallbacksRetireOutsideMetadataLock) {
    NEngine::Engine engine{{.MaxActiveFrames = 1, .WorkerCount = 1}};
    NEngine::Engine other{{.MaxActiveFrames = 1, .WorkerCount = 1}};
    engine.Start();
    other.Start();
    std::atomic<int> destroyed{0};
    EXPECT_FALSE(engine.ScheduleUserUpdate({}, MakeCallback(engine, destroyed)));
    EXPECT_EQ(destroyed.load(), 1);
    const auto key = engine.TryBeginFrame();
    ASSERT_TRUE(key);
    EXPECT_FALSE(other.ScheduleUserUpdate(*key, MakeCallback(other, destroyed)));
    EXPECT_EQ(destroyed.load(), 2);
    EXPECT_TRUE(engine.ScheduleUserUpdate(*key, [] {}));
    EXPECT_FALSE(engine.ScheduleUserUpdate(*key, MakeCallback(engine, destroyed)));
    EXPECT_EQ(destroyed.load(), 3);
    engine.Stop();
    EXPECT_FALSE(engine.ScheduleUserUpdate(*key, MakeCallback(engine, destroyed)));
    EXPECT_EQ(destroyed.load(), 4);
    engine.Start();
    EXPECT_FALSE(engine.ScheduleUserUpdate(*key, MakeCallback(engine, destroyed)));
    EXPECT_EQ(destroyed.load(), 5);
    engine.Stop();
    other.Stop();
}

TEST(EngineAdmission, SmallCallbackCapturesRetireBeforeSimulationCompletion) {
    class RetirementRuntime final: public NEngine::NRuntime::IFrameRuntime {
    public:
        std::atomic<int> Destroyed{0};
        std::promise<void> Updated;
        void Update(const NEngine::NRuntime::FrameContext&) override {
            EXPECT_EQ(Destroyed.load(), 1);
            Updated.set_value();
        }
        void Draw(const NEngine::NRuntime::FrameContext&) override {
        }
    };
    auto runtime = std::make_unique<RetirementRuntime>();
    auto& observed = *runtime;
    auto updated = observed.Updated.get_future();
    auto engine =
            NEngine::NRuntime::EngineFactory::Create({.MaxActiveFrames = 1, .WorkerCount = 1}, std::move(runtime));
    engine->Start();
    const auto key = engine->TryBeginFrame();
    ASSERT_TRUE(key);
    EXPECT_TRUE(engine->ScheduleUserUpdate(
            *key,
            [capture = std::make_shared<ReentrantCapture>(engine.get(), &observed.Destroyed)] {}));
    EXPECT_EQ(updated.wait_for(std::chrono::seconds{2}), std::future_status::ready);
    engine->Stop();
    EXPECT_EQ(observed.Destroyed.load(), 1);
}

TEST(EngineAdmission, EverySubmitAllocationFailureRetiresReentrantCaptureOutsideMetadataLock) {
    int failures = 0;
    int successes = 0;
    for (std::ptrdiff_t budget = 0; budget < 100; ++budget) {
        NEngine::Engine engine{{.MaxActiveFrames = 1, .WorkerCount = 1}};
        engine.Start();
        const auto key = engine.TryBeginFrame();
        ASSERT_TRUE(key);
        std::atomic<int> destroyed{0};
        auto callback = MakeCallback(engine, destroyed);
        AllocationBudget = budget;
        try {
            const bool accepted = engine.ScheduleUserUpdate(*key, std::move(callback));
            AllocationBudget = -1;
            EXPECT_TRUE(accepted);
            ++successes;
        } catch (const std::bad_alloc&) {
            AllocationBudget = -1;
            ++failures;
        }
        engine.Stop();
        EXPECT_EQ(destroyed.load(), 1) << "Allocation prefix " << budget;
    }
    EXPECT_GT(failures, 0);
    EXPECT_GT(successes, 0);
}
} // namespace
