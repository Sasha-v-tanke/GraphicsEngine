#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <lib/thread/task/task_system.h>
#include <resources/resource_io.h>
#include <tests/common/test_error.h>

namespace {

using NResources::EResourceIoStatus;
using NResources::ResourceIo;
using NResources::ResourceIoBytes;
using NResources::ResourceIoRequest;
using NResources::ResourceIoResult;
using Result = std::shared_ptr<const ResourceIoResult>;
constexpr std::chrono::seconds TIMEOUT{5};

struct CompletionProbe {
    std::promise<Result> Promise;
    std::atomic_int Count{0};

    ResourceIo::Completion Callback() {
        return [this](Result result) {
            if (Count.fetch_add(1) == 0) {
                Promise.set_value(std::move(result));
            }
        };
    }
};

struct DelayedRead {
    std::mutex Mutex;
    std::condition_variable_any Wakeup;
    bool Released = false;
    std::promise<void> Started;
    std::atomic_int Calls{0};

    ResourceIoBytes Read(const std::filesystem::path&, std::stop_token token, std::size_t) {
        if (Calls.fetch_add(1) == 0) {
            Started.set_value();
        }
        std::unique_lock lock{Mutex};
        Wakeup.wait(lock, std::move(token), [this] { return Released; });
        return {std::byte{42}};
    }

    void Release() {
        {
            std::lock_guard lock{Mutex};
            Released = true;
        }
        Wakeup.notify_all();
    }

    ResourceIo::ReadFunction Function() {
        return [this](const std::filesystem::path& path, std::stop_token token, std::size_t limit) {
            return Read(path, std::move(token), limit);
        };
    }
};

class ResourceIoTest: public testing::Test {
protected:
    void SetUp() override {
        for (std::size_t index = 0;; ++index) {
            Directory = std::filesystem::temp_directory_path() / ("graphics-resource-io-" + std::to_string(index));
            if (std::filesystem::create_directory(Directory)) {
                break;
            }
        }
        Path = Directory / "asset.bin";
    }

    void TearDown() override {
        std::filesystem::remove_all(Directory);
    }

    static Result ReadFile(ResourceIo& io, const std::filesystem::path& path) {
        const auto probe = std::make_shared<CompletionProbe>();
        std::future<Result> done = probe->Promise.get_future();
        const ResourceIoRequest request =
                io.Read(path, [probe](Result result) { probe->Callback()(std::move(result)); });
        EXPECT_TRUE(request.IsValid());
        if (done.wait_for(TIMEOUT) != std::future_status::ready) {
            ADD_FAILURE() << "I/O completion timed out";
            return {};
        }
        return done.get();
    }

    std::filesystem::path Directory;
    std::filesystem::path Path;
};

TEST_F(ResourceIoTest, ReadsBinaryAndEmptyFilesAndRetainsResultAfterServiceDestruction) {
    {
        std::ofstream file{Path, std::ios::binary};
        const char data[] = {'a', '\0', 'b'};
        file.write(data, sizeof(data));
    }
    Result retained;
    ResourceIoRequest retainedRequest;
    {
        ResourceIo io;
        retained = ReadFile(io, Path);
        ASSERT_NE(retained, nullptr);
        EXPECT_EQ(retained->Status, EResourceIoStatus::SUCCEEDED);
        ASSERT_NE(retained->Bytes, nullptr);
        EXPECT_EQ(*retained->Bytes, (ResourceIoBytes{std::byte{'a'}, std::byte{0}, std::byte{'b'}}));
        EXPECT_FALSE(retained->Failure.has_value());
        std::ofstream{Path, std::ios::trunc}.close();
        const Result empty = ReadFile(io, Path);
        ASSERT_NE(empty, nullptr);
        ASSERT_NE(empty->Bytes, nullptr);
        EXPECT_TRUE(empty->Bytes->empty());
        retainedRequest = io.Read(Path);
        io.Shutdown();
    }
    EXPECT_EQ(retained->Bytes->size(), 3);
    ASSERT_NE(retainedRequest.GetResult(), nullptr);
    retainedRequest.Cancel();
    EXPECT_NE(retainedRequest.GetResult(), nullptr);
}

TEST_F(ResourceIoTest, ReportsMissingDirectoryAndByteLimitFailuresWithPathContext) {
    ResourceIo io{{.WorkerCount = 1, .MaxPendingReads = 4, .MaxReadBytes = 3}};
    for (const std::filesystem::path& path: {Path, Directory}) {
        const Result result = ReadFile(io, path);
        ASSERT_NE(result, nullptr);
        EXPECT_EQ(result->Status, EResourceIoStatus::FAILED);
        EXPECT_EQ(result->Bytes, nullptr);
        ASSERT_TRUE(result->Failure.has_value());
        EXPECT_NE(result->Failure->Message.find(path.string()), std::string::npos);
    }
    {
        std::ofstream file{Path};
        file << "1234";
    }
    const Result oversized = ReadFile(io, Path);
    ASSERT_NE(oversized, nullptr);
    EXPECT_EQ(oversized->Status, EResourceIoStatus::FAILED);
    EXPECT_EQ(oversized->Bytes, nullptr);
    EXPECT_EQ(oversized->Failure->Code, NCommon::make_error_code(NCommon::EError::INVALID_ARGUMENT));
    io.Shutdown();
}

TEST(ResourceIo, DelayedReadDoesNotOccupyComputeWorkerAndHandsOffOwnedBytes) {
    DelayedRead delayed;
    std::future<void> started = delayed.Started.get_future();
    std::promise<NCommon::TaskHandle> submitted;
    std::future<NCommon::TaskHandle> task = submitted.get_future();
    std::atomic_bool parsed{false};
    NCommon::TaskSystem tasks{1};
    ResourceIo io{{.WorkerCount = 1}, delayed.Function()};
    const auto request = io.Read("delayed", [&](const Result& result) {
        EXPECT_FALSE(NCommon::TaskSystem::IsCurrentThreadWorker());
        submitted.set_value(tasks.Submit([result, &parsed](NCommon::TaskContext&) {
            EXPECT_TRUE(NCommon::TaskSystem::IsCurrentThreadWorker());
            EXPECT_EQ(result->Status, EResourceIoStatus::SUCCEEDED);
            parsed = result->Bytes && result->Bytes->front() == std::byte{42};
        }));
    });
    ASSERT_EQ(started.wait_for(TIMEOUT), std::future_status::ready);
    EXPECT_EQ(request.GetResult(), nullptr);
    std::promise<void> computeProgress;
    std::future<void> progress = computeProgress.get_future();
    const auto independent = tasks.Submit([&](NCommon::TaskContext&) { computeProgress.set_value(); });
    EXPECT_EQ(progress.wait_for(TIMEOUT), std::future_status::ready);
    tasks.Wait(independent);
    EXPECT_FALSE(parsed.load());
    delayed.Release();
    ASSERT_EQ(task.wait_for(TIMEOUT), std::future_status::ready);
    tasks.Wait(task.get());
    EXPECT_TRUE(parsed.load());
    io.Shutdown();
}

TEST(ResourceIo, CancelsQueuedRequestWithoutInvokingReader) {
    DelayedRead delayed;
    CompletionProbe first;
    CompletionProbe second;
    std::future<void> started = delayed.Started.get_future();
    std::future<Result> firstResult = first.Promise.get_future();
    std::future<Result> secondResult = second.Promise.get_future();
    ResourceIo io{{.WorkerCount = 1}, delayed.Function()};
    const auto active = io.Read("active", first.Callback());
    ASSERT_EQ(started.wait_for(TIMEOUT), std::future_status::ready);
    const auto queued = io.Read("queued", second.Callback());
    queued.Cancel();
    delayed.Release();
    ASSERT_EQ(firstResult.wait_for(TIMEOUT), std::future_status::ready);
    ASSERT_EQ(secondResult.wait_for(TIMEOUT), std::future_status::ready);
    EXPECT_EQ(firstResult.get()->Status, EResourceIoStatus::SUCCEEDED);
    EXPECT_EQ(secondResult.get()->Status, EResourceIoStatus::CANCELLED);
    EXPECT_EQ(delayed.Calls.load(), 1);
    io.Shutdown();
    EXPECT_EQ(first.Count.load(), 1);
    EXPECT_EQ(second.Count.load(), 1);
    EXPECT_NE(active.GetResult(), nullptr);
}

TEST(ResourceIo, CancelsActiveReadWithoutShuttingDownService) {
    DelayedRead delayed;
    CompletionProbe completion;
    std::future<void> started = delayed.Started.get_future();
    std::future<Result> done = completion.Promise.get_future();
    ResourceIo io{{.WorkerCount = 1}, delayed.Function()};
    const auto active = io.Read("active", completion.Callback());
    ASSERT_EQ(started.wait_for(TIMEOUT), std::future_status::ready);
    active.Cancel();
    ASSERT_EQ(done.wait_for(TIMEOUT), std::future_status::ready);
    const Result result = done.get();
    EXPECT_EQ(result->Status, EResourceIoStatus::CANCELLED);
    EXPECT_EQ(result->Bytes, nullptr);
    EXPECT_FALSE(result->Failure.has_value());
    delayed.Release();
    CompletionProbe later;
    std::future<Result> next = later.Promise.get_future();
    const auto second = io.Read("next", later.Callback());
    ASSERT_EQ(next.wait_for(TIMEOUT), std::future_status::ready);
    EXPECT_EQ(next.get()->Status, EResourceIoStatus::SUCCEEDED);
    io.Shutdown();
    EXPECT_EQ(completion.Count.load(), 1);
    EXPECT_EQ(second.GetResult()->Status, EResourceIoStatus::SUCCEEDED);
}

TEST(ResourceIo, RunsAtMostConfiguredNumberOfReadsAndAcceptsConcurrentShutdown) {
    std::atomic_int active{0};
    std::atomic_int maximum{0};
    std::promise<void> bothStarted;
    std::future<void> started = bothStarted.get_future();
    std::mutex mutex;
    std::condition_variable_any wakeup;
    ResourceIo io{{.WorkerCount = 2, .MaxPendingReads = 8},
                  [&](const std::filesystem::path&, std::stop_token token, std::size_t) {
                      const int current = active.fetch_add(1) + 1;
                      int previous = maximum.load();
                      while (current > previous && !maximum.compare_exchange_weak(previous, current)) {
                      }
                      if (current == 2) {
                          bothStarted.set_value();
                      }
                      std::unique_lock lock{mutex};
                      wakeup.wait(lock, std::move(token), [] { return false; });
                      --active;
                      return ResourceIoBytes{};
                  }};
    std::vector<ResourceIoRequest> requests;
    requests.reserve(8);
    for (int index = 0; index < 8; ++index) {
        requests.push_back(io.Read("pending"));
    }
    ASSERT_EQ(started.wait_for(TIMEOUT), std::future_status::ready);
    std::thread otherShutdown{[&io] { io.Shutdown(); }};
    io.Shutdown();
    otherShutdown.join();
    EXPECT_EQ(maximum.load(), 2);
    EXPECT_EQ(active.load(), 0);
    for (const ResourceIoRequest& request: requests) {
        ASSERT_NE(request.GetResult(), nullptr);
        EXPECT_EQ(request.GetResult()->Status, EResourceIoStatus::CANCELLED);
    }
}

TEST(ResourceIo, ShutdownCancelsActiveAndPendingReadsDrainsCompletionsAndRejectsAdmission) {
    DelayedRead delayed;
    CompletionProbe active;
    CompletionProbe queued;
    std::future<void> started = delayed.Started.get_future();
    ResourceIo io{{.WorkerCount = 1}, delayed.Function()};
    const auto first = io.Read("active", active.Callback());
    ASSERT_EQ(started.wait_for(TIMEOUT), std::future_status::ready);
    const auto second = io.Read("queued", queued.Callback());
    io.Shutdown();
    io.Shutdown();
    ASSERT_NE(first.GetResult(), nullptr);
    ASSERT_NE(second.GetResult(), nullptr);
    EXPECT_EQ(first.GetResult()->Status, EResourceIoStatus::CANCELLED);
    EXPECT_EQ(first.GetResult()->Bytes, nullptr);
    EXPECT_EQ(second.GetResult()->Status, EResourceIoStatus::CANCELLED);
    EXPECT_EQ(active.Count.load(), 1);
    EXPECT_EQ(queued.Count.load(), 1);
    EXPECT_EQ(delayed.Calls.load(), 1);
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(io.Read("rejected")); });
}

TEST(ResourceIo, BoundsAcceptedWorkAndWorkerConcurrency) {
    DelayedRead delayed;
    std::future<void> started = delayed.Started.get_future();
    ResourceIo io{{.WorkerCount = 1, .MaxPendingReads = 2}, delayed.Function()};
    const auto first = io.Read("first");
    ASSERT_EQ(started.wait_for(TIMEOUT), std::future_status::ready);
    const auto second = io.Read("second");
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(io.Read("over-capacity")); });
    EXPECT_EQ(delayed.Calls.load(), 1);
    delayed.Release();
    io.Shutdown();
    EXPECT_NE(first.GetResult(), nullptr);
    EXPECT_NE(second.GetResult(), nullptr);
}

TEST(ResourceIo, ContainsReaderAndCompletionExceptionsAndKeepsWorkerUsable) {
    ResourceIo io{{.WorkerCount = 1}, [](const std::filesystem::path& path, const std::stop_token&, std::size_t) {
                      if (path == "error") {
                          throw std::runtime_error("read failed");
                      }
                      return ResourceIoBytes{std::byte{1}};
                  }};
    CompletionProbe failure;
    std::future<Result> failed = failure.Promise.get_future();
    const auto bad = io.Read("error", failure.Callback());
    ASSERT_EQ(failed.wait_for(TIMEOUT), std::future_status::ready);
    EXPECT_EQ(failed.get()->Status, EResourceIoStatus::FAILED);
    EXPECT_NE(bad.GetResult()->Failure->Message.find("read failed"), std::string::npos);
    std::promise<void> entered;
    std::future<void> callback = entered.get_future();
    const auto throwing = io.Read("success", [&](const Result&) {
        entered.set_value();
        throw std::runtime_error("callback");
    });
    ASSERT_EQ(callback.wait_for(TIMEOUT), std::future_status::ready);
    CompletionProbe later;
    std::future<Result> recovered = later.Promise.get_future();
    const auto next = io.Read("after-exception", later.Callback());
    ASSERT_EQ(recovered.wait_for(TIMEOUT), std::future_status::ready);
    EXPECT_EQ(recovered.get()->Status, EResourceIoStatus::SUCCEEDED);
    io.Shutdown();
    EXPECT_NE(next.GetResult(), nullptr);
    EXPECT_EQ(throwing.GetResult()->Status, EResourceIoStatus::SUCCEEDED);
    ASSERT_TRUE(throwing.GetCompletionFailure().has_value());
    EXPECT_EQ(throwing.GetCompletionFailure()->Code, NCommon::make_error_code(NCommon::EError::UNKNOWN));
}

TEST(ResourceIo, CallbacksMayObserveRequestsAndRejectSelfJoinWithoutDeadlocking) {
    std::promise<void> entered;
    std::future<void> done = entered.get_future();
    ResourceIo io{{.WorkerCount = 1},
                  [](const std::filesystem::path&, const std::stop_token&, std::size_t) { return ResourceIoBytes{}; }};
    const auto request = io.Read("empty", [&](const Result&) {
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { io.Shutdown(); });
        entered.set_value();
    });
    ASSERT_EQ(done.wait_for(TIMEOUT), std::future_status::ready);
    io.Shutdown();
    EXPECT_EQ(request.GetResult()->Status, EResourceIoStatus::SUCCEEDED);
}

TEST(ResourceIo, ReleasesCompletionCapturesOutsideQueueLock) {
    DelayedRead delayed;
    std::future<void> started = delayed.Started.get_future();
    std::promise<void> destroyed;
    std::future<void> done = destroyed.get_future();
    ResourceIo io{{.WorkerCount = 1, .MaxPendingReads = 4}, delayed.Function()};
    auto capture = std::shared_ptr<int>{new int{0}, [&](const int* value) {
                                            delete value;
                                            try {
                                                static_cast<void>(io.Read("reentrant-admission"));
                                            } catch (const NCommon::Exception& exception) {
                                                EXPECT_EQ(exception.code(),
                                                          NCommon::make_error_code(NCommon::EError::INVALID_STATE));
                                            }
                                            destroyed.set_value();
                                        }};
    const auto request = io.Read("first", [owned = capture](const Result&) { EXPECT_EQ(*owned, 0); });
    ASSERT_EQ(started.wait_for(TIMEOUT), std::future_status::ready);
    // The callback must be the final capture owner when the worker releases it.
    capture.reset();
    delayed.Release();
    ASSERT_EQ(done.wait_for(TIMEOUT), std::future_status::ready);
    io.Shutdown();
    EXPECT_NE(request.GetResult(), nullptr);
}

TEST(ResourceIo, EnforcesByteLimitForInjectedReadersAndContainsUnknownExceptions) {
    ResourceIo io{{.WorkerCount = 1, .MaxReadBytes = 1},
                  [](const std::filesystem::path& path, const std::stop_token&, std::size_t) {
                      if (path == "unknown") {
                          throw 7;
                      }
                      return ResourceIoBytes(2);
                  }};
    CompletionProbe oversized;
    CompletionProbe unknown;
    std::future<Result> first = oversized.Promise.get_future();
    std::future<Result> second = unknown.Promise.get_future();
    const auto large = io.Read("large", oversized.Callback());
    const auto bad = io.Read("unknown", unknown.Callback());
    ASSERT_EQ(first.wait_for(TIMEOUT), std::future_status::ready);
    ASSERT_EQ(second.wait_for(TIMEOUT), std::future_status::ready);
    EXPECT_EQ(first.get()->Failure->Code, NCommon::make_error_code(NCommon::EError::INVALID_ARGUMENT));
    EXPECT_EQ(second.get()->Failure->Code, NCommon::make_error_code(NCommon::EError::UNKNOWN));
    io.Shutdown();
    EXPECT_EQ(large.GetResult()->Bytes, nullptr);
    EXPECT_EQ(bad.GetResult()->Bytes, nullptr);
}

TEST(ResourceIo, ValidatesConfigurationAndEmptyHandles) {
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [] { ResourceIo io{{.WorkerCount = 0}}; });
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [] { ResourceIo io{{.MaxPendingReads = 0}}; });
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [] { ResourceIo io{{.MaxReadBytes = 0}}; });
    ResourceIo io;
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { static_cast<void>(io.Read({})); });
    const ResourceIoRequest empty;
    EXPECT_FALSE(empty.IsValid());
    EXPECT_EQ(empty.GetResult(), nullptr);
    EXPECT_FALSE(empty.GetCompletionFailure().has_value());
    empty.Cancel();
}

} // namespace
