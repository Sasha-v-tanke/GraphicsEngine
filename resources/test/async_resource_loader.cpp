#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>

#include <gtest/gtest.h>
#include <resources/async_resource_loader.h>
#include <resources/image_loader.h>
#include <resources/mesh_loader.h>

namespace {

using NResources::AsyncResourceLoader;
using NResources::EResourceState;
using NResources::ResourceIdentity;
using NResources::ResourceUpload;

struct Gate {
    std::mutex Mutex;
    std::condition_variable_any Wake;
    bool Opened = false;
    std::atomic_bool Entered = false;

    void Wait(const std::stop_token& stop) {
        Entered = true;
        std::unique_lock lock{Mutex};
        Wake.wait(lock, stop, [&] { return Opened; });
    }
    void Open() {
        std::lock_guard lock{Mutex};
        Opened = true;
        Wake.notify_all();
    }
};

template<typename Predicate>
bool Until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

class AsyncResources: public testing::Test {
protected:
    NResources::ResourceManager Resources;
    NCommon::TaskSystem Tasks{1};
    Gate ReadGate;
    Gate DecodeGate;
    std::atomic_int Reads = 0;
    std::atomic_int Decodes = 0;
    bool DelayRead = false;
    bool DelayDecode = false;
    std::atomic_bool ReadFails = false;
    std::atomic_bool DecodeFails = false;
    std::thread::id DecodeThread;

    NResources::ResourceIo::ReadFunction Reader() {
        return [this](const auto&, const std::stop_token& stop, std::size_t) {
            ++Reads;
            if (DelayRead) {
                ReadGate.Wait(stop);
            }
            if (ReadFails) {
                throw std::runtime_error("read failed");
            }
            return NResources::ResourceIoBytes{std::byte{42}};
        };
    }
    AsyncResourceLoader::Decoder<int> Decoder() {
        return [this](auto bytes, const std::stop_token& stop) {
            ++Decodes;
            DecodeThread = std::this_thread::get_id();
            if (DelayDecode) {
                DecodeGate.Wait(stop);
            }
            if (DecodeFails) {
                throw std::runtime_error("decode failed");
            }
            return std::make_shared<const int>(std::to_integer<int>(bytes[0]));
        };
    }
};

TEST_F(AsyncResources, ConcurrentIdentityRequestsReadAndDecodeOnceWithoutBlockingCompute) {
    DelayRead = true;
    AsyncResourceLoader loader{Resources, Tasks, {}, Reader()};
    std::vector<NResources::ResourceHandle<int>> handles(24);
    std::vector<std::jthread> callers;
    callers.reserve(handles.size());
    for (auto& handle: handles) {
        callers.emplace_back([&] { handle = loader.Load<int>({"test", "one"}, "file", Decoder()); });
    }
    callers.clear();
    for (auto handle: handles) {
        EXPECT_EQ(handle, handles[0]);
    }
    ASSERT_TRUE(Until([&] { return ReadGate.Entered.load(); }));
    EXPECT_EQ(Reads, 1);
    EXPECT_EQ(Decodes, 0);
    std::promise<void> progress;
    const auto progressTask = Tasks.Submit([&](NCommon::TaskContext&) { progress.set_value(); });
    EXPECT_EQ(progress.get_future().wait_for(std::chrono::seconds{2}), std::future_status::ready);
    Tasks.Wait(progressTask);
    ReadGate.Open();
    ASSERT_TRUE(Until([&] {
        loader.PumpUploads();
        return Resources.GetState(handles[0]) == EResourceState::READY;
    }));
    EXPECT_EQ(Decodes, 1);
    EXPECT_NE(DecodeThread, std::this_thread::get_id());
    EXPECT_EQ(**Resources.TryAcquire(handles[0]), 42);
}

TEST_F(AsyncResources, CpuReadyAndExactGpuCompletionAreSeparateAndLeasePinsBoth) {
    AsyncResourceLoader loader{Resources, Tasks, {}, Reader()};
    std::atomic_bool complete = false;
    std::atomic_int submits = 0;
    auto gpu = std::make_shared<int>(7);
    std::weak_ptr<int> weakGpu = gpu;
    const auto owner = std::this_thread::get_id();
    auto handle = loader.Load<int>({"test", "gpu"}, "file", Decoder(), [&](const auto& cpu) {
        EXPECT_EQ(std::this_thread::get_id(), owner);
        EXPECT_EQ(*cpu, 42);
        ++submits;
        return ResourceUpload{gpu, [&] { return complete.load(); }, [&] { complete = true; }};
    });
    ASSERT_TRUE(Until([&] { return Resources.GetState(handle) == EResourceState::CPU_READY; }));
    EXPECT_FALSE(Resources.TryAcquire(handle));
    EXPECT_EQ(*Resources.GetCpuResource(handle), 42);
    ASSERT_TRUE(Until([&] {
        loader.PumpUploads();
        return Resources.GetState(handle) == EResourceState::UPLOADING;
    }));
    EXPECT_FALSE(Resources.TryAcquire(handle));
    std::promise<void> progress;
    Tasks.Submit([&](NCommon::TaskContext&) { progress.set_value(); });
    EXPECT_EQ(progress.get_future().wait_for(std::chrono::seconds{2}), std::future_status::ready);
    complete = true;
    loader.PumpUploads();
    auto lease = Resources.TryAcquire(handle);
    ASSERT_TRUE(lease);
    EXPECT_EQ(lease->GetGpuRepresentation().get(), gpu.get());
    auto record = lease->GetUseRecord();
    gpu.reset();
    Resources.RequestUnload(handle);
    Resources.CompleteUnload(handle);
    loader.Shutdown();
    EXPECT_FALSE(weakGpu.expired());
    lease.reset();
    EXPECT_FALSE(weakGpu.expired());
    record = {};
    EXPECT_TRUE(weakGpu.expired());
    EXPECT_EQ(submits, 1);
}

TEST_F(AsyncResources, CancelDelayedReadThenRetryRejectsOldResult) {
    DelayRead = true;
    AsyncResourceLoader loader{Resources, Tasks, {}, Reader()};
    auto handle = loader.Load<int>({"test", "cancel-read"}, "file", Decoder());
    ASSERT_TRUE(Until([&] { return ReadGate.Entered.load(); }));
    loader.Cancel(handle);
    EXPECT_EQ(Resources.GetState(handle), EResourceState::UNLOADED);
    EXPECT_EQ(loader.Load<int>({"test", "cancel-read"}, "file", Decoder()), handle);
    ReadGate.Open();
    ASSERT_TRUE(Until([&] {
        loader.PumpUploads();
        return Resources.GetState(handle) == EResourceState::READY;
    }));
    EXPECT_EQ(Reads, 2);
    EXPECT_EQ(Decodes, 1);
}

TEST_F(AsyncResources, UnloadAndForgetDuringDecodeCannotPublishIntoReusedSlot) {
    DelayDecode = true;
    AsyncResourceLoader loader{Resources, Tasks, {}, Reader()};
    auto old = loader.Load<int>({"test", "old"}, "file", Decoder());
    ASSERT_TRUE(Until([&] { return DecodeGate.Entered.load(); }));
    Resources.RequestUnload(old);
    Resources.CompleteUnload(old);
    Resources.Forget(old);
    const auto replacement = Resources.Request<int>({"test", "new"});
    EXPECT_EQ(old.GetSlotIndex(), replacement.GetSlotIndex());
    DecodeGate.Open();
    loader.Shutdown();
    EXPECT_EQ(Resources.GetState(replacement), EResourceState::UNLOADED);
    EXPECT_FALSE(Resources.TryAcquire(replacement));
}

TEST_F(AsyncResources, CancelDecodeIsCooperativeAndShutdownJoinsOwnWork) {
    DelayDecode = true;
    AsyncResourceLoader loader{Resources, Tasks, {}, Reader()};
    auto handle = loader.Load<int>({"test", "decode"}, "file", Decoder());
    ASSERT_TRUE(Until([&] { return DecodeGate.Entered.load(); }));
    loader.Cancel(handle);
    loader.Shutdown();
    EXPECT_EQ(Resources.GetState(handle), EResourceState::UNLOADED);
    EXPECT_FALSE(Resources.TryAcquire(handle));
}

TEST_F(AsyncResources, CancelUploadRetainsSubmissionAndOldCompletionCannotReadyRetry) {
    AsyncResourceLoader loader{Resources, Tasks, {}, Reader()};
    auto gpu = std::make_shared<int>(3);
    std::weak_ptr<int> weak = gpu;
    bool complete = false;
    int drains = 0;
    auto handle = loader.Load<int>({"test", "upload"}, "file", Decoder(), [&](const auto&) {
        return ResourceUpload{gpu,
                              [&] { return complete; },
                              [&] {
                                  ++drains;
                                  complete = true;
                              }};
    });
    ASSERT_TRUE(Until([&] {
        loader.PumpUploads();
        return Resources.GetState(handle) == EResourceState::UPLOADING;
    }));
    loader.Cancel(handle);
    gpu.reset();
    EXPECT_FALSE(weak.expired());
    EXPECT_EQ(loader.Load<int>({"test", "upload"}, "file", Decoder()), handle);
    ASSERT_TRUE(Until([&] {
        loader.PumpUploads();
        return Resources.GetState(handle) == EResourceState::READY;
    }));
    EXPECT_EQ(*Resources.GetCpuResource(handle), 42);
    EXPECT_FALSE(weak.expired());
    loader.Shutdown();
    EXPECT_EQ(drains, 1);
    EXPECT_TRUE(weak.expired());
}

TEST_F(AsyncResources, ReadDecodeAndUploadFailuresAreRecoverable) {
    AsyncResourceLoader loader{Resources, Tasks, {}, Reader()};
    ReadFails = true;
    auto handle = loader.Load<int>({"test", "retry"}, "file", Decoder());
    ASSERT_TRUE(Until([&] { return Resources.GetState(handle) == EResourceState::FAILED; }));
    EXPECT_TRUE(Resources.GetFailure(handle));
    loader.PumpUploads();
    ReadFails = false;
    DecodeFails = true;
    loader.Load<int>({"test", "retry"}, "file", Decoder());
    ASSERT_TRUE(Until([&] { return Resources.GetState(handle) == EResourceState::FAILED; }));
    DecodeFails = false;
    loader.PumpUploads();
    loader.Load<int>({"test", "retry"}, "file", Decoder(), [](const auto&) -> ResourceUpload {
        throw std::runtime_error("submit failed");
    });
    ASSERT_TRUE(Until([&] {
        loader.PumpUploads();
        return Resources.GetState(handle) == EResourceState::FAILED;
    }));
    loader.Load<int>({"test", "retry"}, "file", Decoder());
    ASSERT_TRUE(Until([&] {
        loader.PumpUploads();
        return Resources.GetState(handle) == EResourceState::READY;
    }));
}

TEST_F(AsyncResources, CompletionFailureIsFailedAndSubmissionStillDrains) {
    AsyncResourceLoader loader{Resources, Tasks, {}, Reader()};
    int drains = 0;
    auto handle = loader.Load<int>({"test", "poll"}, "file", Decoder(), [&](const auto&) {
        return ResourceUpload{std::make_shared<int>(0),
                              []() -> bool { throw std::runtime_error("device error"); },
                              [&] { ++drains; }};
    });
    ASSERT_TRUE(Until([&] {
        loader.PumpUploads();
        return Resources.GetState(handle) == EResourceState::FAILED;
    }));
    EXPECT_FALSE(Resources.TryAcquire(handle));
    loader.Shutdown();
    EXPECT_EQ(drains, 1);
}

TEST_F(AsyncResources, FullAndStoppedAdmissionLeaveRecoverableFailure) {
    DelayRead = true;
    AsyncResourceLoader loader{Resources, Tasks, {.WorkerCount = 1, .MaxPendingReads = 1}, Reader()};
    auto first = loader.Load<int>({"test", "first"}, "file", Decoder());
    auto second = loader.Load<int>({"test", "second"}, "file", Decoder());
    EXPECT_EQ(Resources.GetState(second), EResourceState::FAILED);
    loader.Shutdown();
    EXPECT_EQ(Resources.GetState(first), EResourceState::UNLOADED);
    loader.Load<int>({"test", "second"}, "file", Decoder());
    EXPECT_EQ(Resources.GetState(second), EResourceState::FAILED);
}

TEST_F(AsyncResources, MemoryMeshDecoderUsesSameImportRulesOnComputeLane) {
    const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    AsyncResourceLoader loader{Resources, Tasks, {}, [&](const auto&, const auto&, auto) {
                                   const auto bytes = std::as_bytes(std::span{obj});
                                   return NResources::ResourceIoBytes{bytes.begin(), bytes.end()};
                               }};
    auto handle = loader.Load<NResources::MeshData>({"mesh", "triangle"}, "file", [](auto bytes, const auto&) {
        return NResources::DecodeMesh(bytes);
    });
    ASSERT_TRUE(Until([&] {
        loader.PumpUploads();
        return Resources.GetState(handle) == EResourceState::READY;
    }));
    EXPECT_TRUE(Resources.TryAcquire(handle));
}

TEST_F(AsyncResources, SeparateLoadersCoalesceAndCpuReadyCancellationSkipsUpload) {
    AsyncResourceLoader first{Resources, Tasks, {}, Reader()};
    AsyncResourceLoader second{Resources, Tasks, {}, Reader()};
    int uploads = 0;
    auto handle = first.Load<int>({"test", "shared"}, "file", Decoder(), [&](const auto&) {
        ++uploads;
        return ResourceUpload{};
    });
    EXPECT_EQ(second.Load<int>({"test", "shared"}, "file", Decoder()), handle);
    ASSERT_TRUE(Until([&] { return Resources.GetState(handle) == EResourceState::CPU_READY; }));
    first.Cancel(handle);
    first.PumpUploads();
    EXPECT_EQ(Resources.GetState(handle), EResourceState::UNLOADED);
    EXPECT_EQ(uploads, 0);
    EXPECT_EQ(Reads, 1);
    EXPECT_EQ(Decodes, 1);
}

TEST_F(AsyncResources, ShutdownCancelsQueuedDecodeWithoutWaitingForUnrelatedRunningTask) {
    Gate unrelated;
    const auto task = Tasks.Submit([&](NCommon::TaskContext&) { unrelated.Wait({}); });
    ASSERT_TRUE(Until([&] { return unrelated.Entered.load(); }));
    AsyncResourceLoader loader{Resources, Tasks, {}, Reader()};
    const auto handle = loader.Load<int>({"test", "queued"}, "file", Decoder());
    const bool loading = Until([&] { return Resources.GetState(handle) == EResourceState::LOADING; });
    // Release the unrelated task from another host thread if Shutdown accidentally waits for it.
    std::atomic_bool shutdownDone = false;
    std::jthread guard{[&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (!shutdownDone && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        unrelated.Open();
    }};
    const auto start = std::chrono::steady_clock::now();
    loader.Shutdown();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    shutdownDone = true;
    guard.join();
    Tasks.Wait(task);
    EXPECT_TRUE(loading);
    EXPECT_LT(elapsed, std::chrono::seconds{1});
    EXPECT_EQ(Decodes, 0);
    EXPECT_EQ(Resources.GetState(handle), EResourceState::UNLOADED);
}

TEST_F(AsyncResources, BuiltinImageAndShaderDecodeOnAsyncPipeline) {
    const std::string image = "P6\n1 1\n255\n\x11\x22\x33";
    const std::array<std::uint32_t, 5> words{0x07230203, 0x00010000, 0, 1, 0};
    AsyncResourceLoader loader{Resources, Tasks, {}, [&](const auto& path, const auto&, auto) {
                                   auto bytes = path == "image" ? std::as_bytes(std::span{image})
                                                                : std::as_bytes(std::span{words});
                                   return NResources::ResourceIoBytes{bytes.begin(), bytes.end()};
                               }};
    const auto texture = loader.LoadImage({"image", "one-pixel"}, "image");
    const auto shader = loader.LoadShaderArtifact({"shader", "vertex"}, "shader", NResources::EShaderStage::VERTEX);
    ASSERT_TRUE(Until([&] {
        loader.PumpUploads();
        return Resources.GetState(texture) == EResourceState::READY &&
               Resources.GetState(shader) == EResourceState::READY;
    }));
    EXPECT_EQ(Resources.GetCpuResource(texture)->GetPixels(), (std::vector<std::uint8_t>{0x11, 0x22, 0x33, 0xff}));
    EXPECT_TRUE(Resources.TryAcquire(shader));
}

} // namespace
