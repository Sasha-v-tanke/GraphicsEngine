#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <benchmark/benchmark.h>
#include <graphics/vulkan/device.h>
#include <graphics/vulkan/glfw_surface.h>
#include <graphics/vulkan/instance.h>
#include <graphics/vulkan/memory.h>
#include <graphics/vulkan/physical_device.h>
#include <graphics/vulkan/resource_state.h>
#include <graphics/vulkan/submission_manager.h>
#include <graphics/vulkan/upload_manager.h>
#include <lib/common/error/exception.h>
#include <window/window.h>
#include <window/window_config.h>
#include <window/window_runtime.h>
#include <window/window_type.h>

namespace {

void BM_UploadBatches(benchmark::State& state) {
    try {
        NWindow::WindowRuntime runtime;
        NWindow::WindowConfig config{NWindow::EWindowType::GLFW};
        config.Title = "Upload benchmark";
        config.Size = {.Width = 64, .Height = 64};
        NWindow::Window window{config};
        NVulkan::VulkanInstanceConfig instanceConfig;
        instanceConfig.RequiredExtensions = NVulkan::NGlfw::GetRequiredInstanceExtensions();
        instanceConfig.ValidationMode = NVulkan::EValidationMode::Disabled;
        const NVulkan::VulkanInstance instance{instanceConfig};
        const NVulkan::NGlfw::VulkanSurface surface{instance, window};
        const auto physicalDevice = NVulkan::SelectVulkanPhysicalDevice(instance, surface.GetHandle());
        const NVulkan::VulkanDevice device{physicalDevice};
        NVulkan::VulkanResourceStateTracker resourceStates;
        NVulkan::VulkanMemoryAllocator memory{instance.GetHandle(), physicalDevice.Handle, device, resourceStates};
        NVulkan::VulkanSubmissionManager submissions{device};
        NVulkan::VulkanUploadManager uploads{device,
                                             memory,
                                             submissions,
                                             resourceStates,
                                             {.ArenaSizeBytes = 256 * 1024, .MaxStagingBytes = 32 * 1024 * 1024}};
        constexpr std::size_t uploadCount = 64;
        std::array<std::byte, 4096> bytes{};
        std::vector<NVulkan::VulkanBufferAllocation> destinations;
        destinations.reserve(uploadCount);
        for (std::size_t index = 0; index < uploadCount; ++index) {
            destinations.emplace_back(NVulkan::CreateVulkanBuffer(
                    memory,
                    {
                            .SizeBytes = bytes.size(),
                            .Usage = NGraphics::EBufferUsage::TransferDestination | NGraphics::EBufferUsage::Vertex,
                            .Access = NGraphics::BufferAccess(NGraphics::EBufferAccess::GpuRead),
                    }));
        }
        std::vector<NVulkan::VulkanBufferUpload> requests;
        requests.reserve(uploadCount);
        for (auto& destination: destinations) {
            requests.push_back({&destination, bytes});
        }
        const auto batchSize = static_cast<std::size_t>(state.range(0));
        for ([[maybe_unused]] auto iteration: state) {
            NVulkan::VulkanQueueCompletion completion;
            for (std::size_t index = 0; index < uploadCount; index += batchSize) {
                completion = uploads.UploadBatch(std::span{requests}.subspan(index, batchSize));
            }
            // Keep workload and residency bounded; this wait belongs to the benchmark harness.
            submissions.Wait(completion, 5'000'000'000);
            uploads.ReleaseCompleted();
        }
        const auto statistics = uploads.GetStatistics();
        state.counters["uploads/batch"] =
                static_cast<double>(statistics.SubmittedUploads) / static_cast<double>(statistics.Submissions);
        state.counters["submissions/iteration"] =
                static_cast<double>(statistics.Submissions) / static_cast<double>(state.iterations());
        state.counters["arena_allocations"] = static_cast<double>(statistics.ArenaAllocations);
        state.counters["staging_high_water"] = static_cast<double>(statistics.StagingHighWaterBytes);
        state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(uploadCount * bytes.size()));
    } catch (const NCommon::Exception& exception) {
        state.SkipWithError(exception.GetMessage());
    }
}

BENCHMARK(BM_UploadBatches)->Arg(1)->Arg(8)->Arg(64)->UseRealTime();

} // namespace
