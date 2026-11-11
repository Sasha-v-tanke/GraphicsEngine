#include <chrono>
#include <cstddef>
#include <semaphore>
#include <vector>

#include <benchmark/benchmark.h>
#include <lib/thread/task/task_system.h>

namespace {

class PausedWorker final {
public:
    explicit PausedWorker(NCommon::TaskSystem& pool)
        : m_pool(pool) {
        m_pool.Submit([this](NCommon::TaskContext&) {
            m_entered.release();
            m_resume.acquire();
        });
        m_entered.acquire();
    }
    ~PausedWorker() {
        m_resume.release();
        m_pool.WaitIdle();
    }

private:
    NCommon::TaskSystem& m_pool;
    std::binary_semaphore m_entered{0};
    std::binary_semaphore m_resume{0};
};

void BM_TaskSystemDagValidation(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    const bool fanOut = state.range(1) != 0;
    for ([[maybe_unused]] auto _: state) {
        NCommon::TaskSystem pool{1};
        PausedWorker paused{pool};
        std::vector<NCommon::TaskHandle> tasks;
        tasks.reserve(count);
        const auto start = std::chrono::steady_clock::now();
        tasks.push_back(pool.Submit([](NCommon::TaskContext&) {}));
        for (std::size_t index = 1; index < count; ++index) {
            const auto& parent = fanOut ? tasks.front() : tasks.back();
            tasks.push_back(pool.Submit([](NCommon::TaskContext&) {}, std::span{&parent, 1}));
        }
        const auto admitted = std::chrono::steady_clock::now();
        pool.Cancel(tasks.front());
        const auto cancelled = std::chrono::steady_clock::now();
        state.SetIterationTime(std::chrono::duration<double>(admitted - start).count());
        state.counters["cancel_ns"] = std::chrono::duration<double, std::nano>(cancelled - admitted).count();
    }
    state.counters["tasks"] = static_cast<double>(count);
}

} // namespace

BENCHMARK(BM_TaskSystemDagValidation)
        ->Args({1000, 0})
        ->Args({4000, 0})
        ->Args({16000, 0})
        ->Args({1000, 1})
        ->Args({4000, 1})
        ->Args({16000, 1})
        ->Iterations(1)
        ->UseManualTime()
        ->Unit(benchmark::kMillisecond);
