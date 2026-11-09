#include <atomic>
#include <memory>
#include <thread>

#include <gtest/gtest.h>
#include <resources/resource_manager.h>

namespace {

using NResources::EResourceReferenceStatus;
using NResources::EResourceState;
using NResources::ResourceManager;

TEST(ResourceObservation, OwnsFailureContextAcrossRetryAndForget) {
    ResourceManager resources;
    const auto handle = resources.Request<int>({"mesh", "broken.obj"});
    const auto operation = resources.BeginLoading(handle);
    resources.Fail(operation, {.Code = NCommon::EError::IO_ERROR, .Message = "broken.obj: decode failed"});
    const auto failed = resources.Observe(handle);
    ASSERT_EQ(failed.Status.Reference, EResourceReferenceStatus::VALID);
    ASSERT_TRUE(failed.Status.Identity);
    EXPECT_EQ(failed.Status.Identity->GetKey(), "broken.obj");
    EXPECT_EQ(failed.Status.State, EResourceState::FAILED);
    ASSERT_TRUE(failed.Status.Failure);
    EXPECT_EQ(failed.Status.Failure->Message, "broken.obj: decode failed");
    EXPECT_FALSE(failed.Lease);
    const auto retry = resources.BeginLoading(handle);
    resources.PublishReady(retry, std::make_shared<int>(1));
    const auto ready = resources.Observe(handle);
    ASSERT_TRUE(ready.Lease);
    EXPECT_EQ(**ready.Lease, 1);
    EXPECT_FALSE(ready.Status.Failure);
    resources.RequestUnload(handle);
    resources.CompleteUnload(handle);
    resources.Forget(handle);
    EXPECT_EQ(failed.Status.State, EResourceState::FAILED);
    EXPECT_EQ(failed.Status.Failure->Message, "broken.obj: decode failed");
    EXPECT_EQ(**ready.Lease, 1);
}

TEST(ResourceObservation, DoesNotExposeAnotherManagerOrReusedGeneration) {
    ResourceManager resources;
    ResourceManager other;
    const auto handle = resources.Request<int>({"test", "old"});
    EXPECT_EQ(resources.Observe(NResources::ResourceHandle<int>{}).Status.Reference, EResourceReferenceStatus::EMPTY);
    const auto foreign = other.Observe(handle);
    EXPECT_EQ(foreign.Status.Reference, EResourceReferenceStatus::FOREIGN);
    EXPECT_FALSE(foreign.Status.Identity);
    EXPECT_FALSE(foreign.Status.State);
    EXPECT_FALSE(foreign.Lease);
    resources.Forget(handle);
    const auto replacement = resources.Request<int>({"test", "new"});
    EXPECT_EQ(handle.GetSlotIndex(), replacement.GetSlotIndex());
    const auto stale = resources.Observe(handle);
    EXPECT_EQ(stale.Status.Reference, EResourceReferenceStatus::STALE);
    EXPECT_FALSE(stale.Status.Identity);
    EXPECT_FALSE(stale.Status.State);
    EXPECT_FALSE(stale.Lease);
}

TEST(ResourceObservation, CpuReadyAndUploadAreUnavailableUntilExactPublication) {
    ResourceManager resources;
    const auto handle = resources.Request<int>({"test", "gpu"});
    const auto operation = *resources.TryQueueLoading(handle);
    EXPECT_EQ(resources.Observe(handle).Status.State, EResourceState::QUEUED);
    ASSERT_TRUE(resources.TryAdvance(operation, EResourceState::QUEUED, EResourceState::LOADING));
    auto cpu = std::make_shared<const int>(42);
    ASSERT_TRUE(resources.TryAdvance(operation, EResourceState::LOADING, EResourceState::CPU_READY, cpu));
    const auto decoded = resources.Observe(handle);
    EXPECT_EQ(decoded.Status.State, EResourceState::CPU_READY);
    EXPECT_FALSE(decoded.Lease);
    ASSERT_TRUE(resources.TryAdvance(operation, EResourceState::CPU_READY, EResourceState::UPLOADING));
    EXPECT_FALSE(resources.Observe(handle).Lease);
    auto gpu = std::make_shared<int>(3);
    std::weak_ptr<int> lifetime = gpu;
    ASSERT_TRUE(resources.TryAdvance(operation, EResourceState::UPLOADING, EResourceState::READY, cpu, gpu));
    auto ready = resources.Observe(handle);
    ASSERT_TRUE(ready.Lease);
    EXPECT_EQ(ready.Status.State, EResourceState::READY);
    EXPECT_EQ(ready.Lease->GetGpuRepresentation().get(), gpu.get());
    resources.RequestUnload(handle);
    resources.CompleteUnload(handle);
    gpu.reset();
    EXPECT_FALSE(lifetime.expired());
    ready.Lease.reset();
    EXPECT_TRUE(lifetime.expired());
}

TEST(ResourceObservation, StateAndVersionAreConsistentDuringConcurrentReload) {
    ResourceManager resources;
    const auto handle = resources.Request<int>({"test", "reload"});
    std::atomic_bool done = false;
    std::jthread writer{[&] {
        for (int version = 1; version <= 1000; ++version) {
            const auto operation = resources.BeginLoading(handle);
            resources.PublishReady(operation, std::make_shared<int>(version));
            resources.RequestUnload(handle);
            resources.CompleteUnload(handle);
        }
        done = true;
    }};
    do {
        const auto observed = resources.Observe(handle);
        EXPECT_EQ(observed.Status.Reference, EResourceReferenceStatus::VALID);
        EXPECT_EQ(observed.Lease.has_value(), observed.Status.State == EResourceState::READY);
        if (observed.Lease) {
            EXPECT_EQ(observed.Lease->GetVersion(), static_cast<std::uint64_t>(**observed.Lease));
        }
    } while (!done.load());
}

} // namespace
