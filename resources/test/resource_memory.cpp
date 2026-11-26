#include <array>
#include <atomic>
#include <barrier>
#include <limits>
#include <thread>

#include <gtest/gtest.h>
#include <lib/common/error/exception.h>
#include <resources/image_data.h>
#include <resources/resource_manager.h>
#include <tests/common/test_error.h>

namespace {
using NResources::AddResourceMemory;
using NResources::EImagePixelFormat;
using NResources::EResourceState;
using NResources::ImageData;
using NResources::ResourceLease;
using NResources::ResourceManager;
using NResources::ResourceMemoryBudget;
using NResources::ResourceMemoryReservation;

TEST(ResourceMemory, ZeroAndSmallBudgetsRejectWithoutPublishing) {
    ResourceManager resources{ResourceMemoryBudget{0}};
    auto handle = resources.Request<int>({"test", "one"});
    auto operation = resources.BeginLoading(handle);
    NTest::ExpectError(NCommon::EError::OUT_OF_MEMORY,
                       [&] { resources.PublishReady(operation, std::make_shared<int>(1)); });
    EXPECT_EQ(resources.GetState(handle), EResourceState::LOADING);
    EXPECT_EQ(resources.GetMemorySnapshot().Total.DecodedBytes, 0U);
    resources.SetMemoryBudget({sizeof(int) - 1});
    NTest::ExpectError(NCommon::EError::OUT_OF_MEMORY,
                       [&] { resources.PublishReady(operation, std::make_shared<int>(1)); });
    resources.SetMemoryBudget({sizeof(int)});
    resources.PublishReady(operation, std::make_shared<int>(1));
    auto snapshot = resources.GetMemorySnapshot();
    EXPECT_TRUE(snapshot.AtCapacity);
    EXPECT_EQ(snapshot.Total.EvictableBytes, sizeof(int));
    EXPECT_EQ(snapshot.Total.PinnedBytes, 0U);
    EXPECT_EQ(snapshot.Total.HighWaterBytes, sizeof(int));
}

TEST(ResourceMemory, ReservationConversionDoesNotDoubleChargeAndRejectsForeignTokens) {
    ResourceManager resources{ResourceMemoryBudget{sizeof(int)}};
    auto pending = resources.ReserveDecode("test", sizeof(int));
    EXPECT_EQ(resources.GetMemorySnapshot().Total.PendingDecodeBytes, sizeof(int));
    auto handle = resources.Request<int>({"test", "one"});
    auto operation = resources.BeginLoading(handle);
    ResourceManager foreign;
    auto wrong = foreign.ReserveDecode("test", sizeof(int));
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT,
                       [&] { resources.PublishReady(operation, std::make_shared<int>(1), &wrong); });
    resources.PublishReady(operation, std::make_shared<int>(1), &pending);
    EXPECT_FALSE(pending.IsValid());
    const auto snapshot = resources.GetMemorySnapshot();
    EXPECT_EQ(snapshot.Total.PendingDecodeBytes, 0U);
    EXPECT_EQ(snapshot.Total.DecodedBytes, sizeof(int));
    EXPECT_EQ(snapshot.Total.HighWaterBytes, sizeof(int));
    EXPECT_EQ(snapshot.Classes.front().Counters.HighWaterBytes, sizeof(int));
}

TEST(ResourceMemory, RetiredLeaseAndUseRecordPreventReadmissionUntilLastRelease) {
    ResourceManager resources{ResourceMemoryBudget{sizeof(int)}};
    auto handle = resources.Request<int>({"test", "one"});
    resources.PublishReady(resources.BeginLoading(handle), std::make_shared<int>(1));
    auto lease = resources.TryAcquire(handle);
    auto use = lease->GetUseRecord();
    EXPECT_EQ(resources.GetMemorySnapshot().Total.PinnedBytes, sizeof(int));
    resources.RequestUnload(handle);
    resources.CompleteUnload(handle);
    resources.Forget(handle);
    auto snapshot = resources.GetMemorySnapshot();
    EXPECT_EQ(snapshot.Total.CacheBytes, 0U);
    EXPECT_EQ(snapshot.Total.PinnedBytes, sizeof(int));
    NTest::ExpectError(NCommon::EError::OUT_OF_MEMORY, [&] { static_cast<void>(resources.ReserveDecode("test", 1)); });
    lease.reset();
    EXPECT_EQ(resources.GetMemorySnapshot().Total.DecodedBytes, sizeof(int));
    use = {};
    auto pending = resources.ReserveDecode("test", sizeof(int));
    EXPECT_EQ(resources.GetMemorySnapshot().Total.DecodedBytes, 0U);
}

TEST(ResourceMemory, CpuPointerAndGpuAliasingRemainPinned) {
    ResourceManager resources;
    auto handle = resources.Request<int>({"test", "one"});
    auto op = resources.BeginLoading(handle);
    ASSERT_TRUE(resources.TryAdvance(op,
                                     EResourceState::LOADING,
                                     EResourceState::CPU_READY,
                                     std::make_shared<const int>(1)));
    auto old = resources.GetCpuResource(handle);
    ASSERT_TRUE(resources.TryAdvance(op, EResourceState::CPU_READY, EResourceState::UPLOADING));
    ASSERT_TRUE(
            resources.TryAdvance(op, EResourceState::UPLOADING, EResourceState::READY, old, std::make_shared<int>(2)));
    EXPECT_EQ(resources.GetMemorySnapshot().Total.DecodedBytes, sizeof(int));
    EXPECT_EQ(resources.GetMemorySnapshot().Total.PinnedBytes, sizeof(int));
    old.reset();
    EXPECT_EQ(resources.GetMemorySnapshot().Total.EvictableBytes, sizeof(int));
    auto lease = resources.TryAcquire(handle);
    EXPECT_EQ(resources.GetMemorySnapshot().Total.PinnedBytes, sizeof(int));
    resources.RequestUnload(handle);
    EXPECT_EQ(resources.GetMemorySnapshot().Total.PinnedBytes, sizeof(int));
    lease.reset();
    EXPECT_EQ(resources.GetMemorySnapshot().Total.DecodedBytes, 0U);
}

TEST(ResourceMemory, OverflowAndConcurrentReservationsAreBounded) {
    ResourceManager resources;
    auto huge = resources.ReserveDecode("z", std::numeric_limits<std::size_t>::max());
    EXPECT_EQ(resources.GetMemorySnapshot().Total.HighWaterBytes, std::numeric_limits<std::size_t>::max());
    NTest::ExpectError(NCommon::EError::OUT_OF_MEMORY, [&] { static_cast<void>(resources.ReserveDecode("a", 1)); });
    huge = {};
    resources.SetMemoryBudget({3});
    std::barrier admitted{9};
    std::barrier release{9};
    std::atomic_uint successes{0};
    std::array<std::jthread, 8> threads;
    for (auto& thread: threads) {
        thread = std::jthread([&] {
            ResourceMemoryReservation reservation;
            try {
                reservation = resources.ReserveDecode("a", 1);
                ++successes;
            } catch (const NCommon::Exception&) {
            }
            admitted.arrive_and_wait();
            release.arrive_and_wait();
        });
    }
    admitted.arrive_and_wait();
    EXPECT_EQ(successes.load(), 3U);
    EXPECT_EQ(resources.GetMemorySnapshot().Total.PendingDecodeBytes, 3U);
    release.arrive_and_wait();
    for (auto& thread: threads) {
        thread.join();
    }
    EXPECT_EQ(resources.GetMemorySnapshot().Total.PendingDecodeBytes, 0U);
    EXPECT_EQ(resources.GetMemorySnapshot().Classes.front().ResourceClass, "a");
}

TEST(ResourceMemory, ReentrantDestructionAndLifetimeBeyondManager) {
    ResourceMemoryReservation reservation;
    std::optional<ResourceLease<int>> lease;
    bool destroyed = false;
    {
        ResourceManager resources;
        reservation = resources.ReserveDecode("test", 10);
        auto handle = resources.Request<int>({"test", "one"});
        auto payload = std::shared_ptr<int>(new int(1), [&](const int* value) {
            static_cast<void>(resources.GetMemorySnapshot());
            delete value;
            destroyed = true;
        });
        resources.PublishReady(resources.BeginLoading(handle), std::move(payload));
        resources.RequestUnload(handle);
        EXPECT_TRUE(destroyed);
        resources.CompleteUnload(handle);
        resources.PublishReady(resources.BeginLoading(handle), std::make_shared<int>(2));
        lease = resources.TryAcquire(handle);
    }
    EXPECT_EQ(**lease, 2);
    lease.reset();
    reservation = {};
}

TEST(ResourceMemory, BuiltinCapacityAccountingAndSizeOverflow) {
    ResourceManager resources;
    auto handle = resources.Request<ImageData>({"image", "one"});
    std::vector<std::uint8_t> pixels(4);
    pixels.reserve(128);
    const auto expected = sizeof(ImageData) + pixels.capacity();
    resources.PublishReady(resources.BeginLoading(handle),
                           std::make_shared<ImageData>(1, 1, EImagePixelFormat::RGBA8, std::move(pixels)));
    EXPECT_EQ(resources.GetMemorySnapshot().Total.DecodedBytes, expected);
    EXPECT_THROW(AddResourceMemory(1, std::numeric_limits<std::size_t>::max(), 4), std::overflow_error);
}

TEST(ResourceMemory, AtomicDecoderOwnershipRemainsChargedAfterCancel) {
    ResourceManager resources{ResourceMemoryBudget{sizeof(int)}};
    auto pending = resources.ReserveDecode("test", sizeof(int));
    auto handle = resources.Request<int>({"test", "decoder"});
    auto operation = resources.BeginLoading(handle);
    std::shared_ptr<const int> cpu = std::make_shared<const int>(42);
    ASSERT_TRUE(resources.TryAdvance(operation,
                                     EResourceState::LOADING,
                                     EResourceState::CPU_READY,
                                     cpu,
                                     {},
                                     &pending,
                                     &cpu));
    EXPECT_FALSE(pending.IsValid());
    ASSERT_TRUE(resources.TryCancel(operation));
    EXPECT_EQ(resources.GetMemorySnapshot().Total.PinnedBytes, sizeof(int));
    EXPECT_EQ(*cpu, 42);
    cpu.reset();
    EXPECT_EQ(resources.GetMemorySnapshot().Total.DecodedBytes, 0U);
}

TEST(ResourceMemory, RepublishingOriginalCpuOwnerPreservesChargeWithAndWithoutGpu) {
    for (const bool upload: {false, true}) {
        SCOPED_TRACE(upload);
        ResourceManager resources{ResourceMemoryBudget{sizeof(int)}};
        auto handle = resources.Request<int>({"test", "republish"});
        auto operation = resources.BeginLoading(handle);
        std::shared_ptr<const int> original = std::make_shared<const int>(42);
        ASSERT_TRUE(resources.TryAdvance(operation, EResourceState::LOADING, EResourceState::CPU_READY, original));
        const auto from = upload ? EResourceState::UPLOADING : EResourceState::CPU_READY;
        if (upload) {
            ASSERT_TRUE(resources.TryAdvance(operation, EResourceState::CPU_READY, EResourceState::UPLOADING));
        }
        std::shared_ptr<const void> gpu = upload ? std::make_shared<int>(7) : nullptr;
        std::weak_ptr<const void> weakGpu = gpu;
        ASSERT_TRUE(resources.TryAdvance(operation, from, EResourceState::READY, original, gpu));
        original.reset();
        gpu.reset();
        auto snapshot = resources.GetMemorySnapshot();
        EXPECT_EQ(snapshot.Total.DecodedBytes, sizeof(int));
        EXPECT_EQ(snapshot.Total.CacheBytes, sizeof(int));
        EXPECT_EQ(snapshot.Total.EvictableBytes, sizeof(int));
        EXPECT_EQ(snapshot.Total.HighWaterBytes, sizeof(int));
        NTest::ExpectError(NCommon::EError::OUT_OF_MEMORY,
                           [&] { static_cast<void>(resources.ReserveDecode("test", 1)); });
        auto lease = resources.TryAcquire(handle);
        ASSERT_TRUE(lease);
        EXPECT_EQ(**lease, 42);
        resources.RequestUnload(handle);
        resources.CompleteUnload(handle);
        EXPECT_EQ(resources.GetMemorySnapshot().Total.PinnedBytes, sizeof(int));
        EXPECT_EQ(weakGpu.expired(), !upload);
        lease.reset();
        EXPECT_EQ(resources.GetMemorySnapshot().Total.DecodedBytes, 0U);
        EXPECT_TRUE(weakGpu.expired());
    }
}

} // namespace
