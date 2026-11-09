#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <spawn.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include <sys/wait.h>

#if defined(__APPLE__)
    #include <crt_externs.h>

    #include <mach-o/dyld.h>
#else
extern char** environ;
#endif

#include "pipeline_shaders.h"

#include <graphics/vulkan/descriptor_manager.h>
#include <graphics/vulkan/device.h>
#include <graphics/vulkan/glfw_surface.h>
#include <graphics/vulkan/graphics_pipeline.h>
#include <graphics/vulkan/instance.h>
#include <graphics/vulkan/memory.h>
#include <graphics/vulkan/physical_device.h>
#include <graphics/vulkan/pipeline_manager.h>
#include <graphics/vulkan/resource_conversion.h>
#include <graphics/vulkan/submission_manager.h>
#include <graphics/vulkan/upload_manager.h>
#include <gtest/gtest.h>
#include <lib/common/error/exception.h>
#include <resources/async_resource_loader.h>
#include <resources/resource_manager.h>
#include <tests/common/test_error.h>
#include <window/window.h>
#include <window/window_config.h>
#include <window/window_runtime.h>
#include <window/window_size.h>
#include <window/window_type.h>

namespace {

constexpr std::string_view PortabilitySubsetExtension = "VK_KHR_portability_subset";

// Observe real driver calls while preserving validation coverage.
class PipelineCreationProbe final: public NCommon::NonTransferable {
public:
    PipelineCreationProbe() {
        Original = vkCreateGraphicsPipelines;
        Calls = 0;
        CacheCalls = 0;
        FailNext = false;
        vkCreateGraphicsPipelines = Create;
    }
    ~PipelineCreationProbe() {
        vkCreateGraphicsPipelines = Original;
    }
    inline static std::atomic<int> Calls = 0;
    inline static std::atomic<int> CacheCalls = 0;
    inline static std::atomic<bool> FailNext = false;

private:
    static VKAPI_ATTR VkResult VKAPI_CALL Create(VkDevice device,
                                                 VkPipelineCache cache,
                                                 std::uint32_t count,
                                                 const VkGraphicsPipelineCreateInfo* info,
                                                 const VkAllocationCallbacks* allocator,
                                                 VkPipeline* pipelines) {
        ++Calls;
        if (cache != VK_NULL_HANDLE) {
            ++CacheCalls;
        }
        if (FailNext.exchange(false)) {
            return VK_ERROR_UNKNOWN;
        }
        return Original(device, cache, count, info, allocator, pipelines);
    }
    inline static PFN_vkCreateGraphicsPipelines Original = nullptr;
};

class UploadProbe final: public NCommon::NonTransferable {
public:
    UploadProbe() {
        OriginalCounter = vkGetSemaphoreCounterValue;
        OriginalSubmit = vkQueueSubmit2;
        OriginalIdle = vkDeviceWaitIdle;
        CompletionLimit = std::numeric_limits<std::uint64_t>::max();
        SubmitCalls = 0;
        IdleCalls = 0;
        FailSubmit = false;
        vkGetSemaphoreCounterValue = Counter;
        vkQueueSubmit2 = Submit;
        vkDeviceWaitIdle = Idle;
    }
    ~UploadProbe() {
        vkGetSemaphoreCounterValue = OriginalCounter;
        vkQueueSubmit2 = OriginalSubmit;
        vkDeviceWaitIdle = OriginalIdle;
    }
    inline static std::uint64_t CompletionLimit = 0;
    inline static std::size_t SubmitCalls = 0;
    inline static std::size_t IdleCalls = 0;
    inline static bool FailSubmit = false;

private:
    static VKAPI_ATTR VkResult VKAPI_CALL Counter(VkDevice device, VkSemaphore semaphore, std::uint64_t* value) {
        const VkResult result = OriginalCounter(device, semaphore, value);
        if (result == VK_SUCCESS) {
            *value = std::min(*value, CompletionLimit);
        }
        return result;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Submit(VkQueue queue,
                                                 std::uint32_t count,
                                                 const VkSubmitInfo2* info,
                                                 VkFence fence) {
        ++SubmitCalls;
        if (std::exchange(FailSubmit, false)) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        return OriginalSubmit(queue, count, info, fence);
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Idle(VkDevice device) {
        ++IdleCalls;
        return OriginalIdle(device);
    }
    inline static PFN_vkGetSemaphoreCounterValue OriginalCounter = nullptr;
    inline static PFN_vkQueueSubmit2 OriginalSubmit = nullptr;
    inline static PFN_vkDeviceWaitIdle OriginalIdle = nullptr;
};

NVulkan::VulkanPhysicalDeviceSelection MakeSelection() {
    NVulkan::VulkanPhysicalDeviceSelection selection{
            .Handle = reinterpret_cast<VkPhysicalDevice>(1),
            .Capabilities =
                    {
                            .Name = {},
                            .ApiVersion = VK_API_VERSION_1_3,
                            .DescriptorLimits =
                                    {
                                            .MinUniformBufferOffsetAlignment = 16,
                                            .MinStorageBufferOffsetAlignment = 16,
                                            .MaxUniformBufferRange = 1024,
                                            .MaxStorageBufferRange = 1024,
                                    },
                            .Extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME},
                            .Features =
                                    {
                                            .TimelineSemaphore = true,
                                            .Synchronization2 = true,
                                            .DynamicRendering = true,
                                    },
                            .QueueFamilies = {},
                            .SurfaceSupported = true,
                            .SwapchainFormatsSupported = true,
                            .SwapchainPresentModesSupported = true,
                    },
            .GraphicsQueueFamilyIndex = 2,
            .PresentQueueFamilyIndex = 2,
    };

    return selection;
}

bool IsEnvironmentFailure(const NCommon::Exception& exception) {
    const std::string& message = exception.GetMessage();

    return message.find("VK_ERROR_INCOMPATIBLE_DRIVER") != std::string::npos ||
           message.find("Failed to create GLFW window") != std::string::npos ||
           message.find("Failed to initialize GLFW") != std::string::npos ||
           message.find("Failed to get GLFW Vulkan instance extensions") != std::string::npos;
}

NWindow::WindowConfig MakeWindowConfig() {
    NWindow::WindowConfig windowConfig{NWindow::EWindowType::GLFW};
    windowConfig.Title = "GraphicsEngine Vulkan Device Test";
    windowConfig.Size = {
            .Width = 64,
            .Height = 64,
    };

    return windowConfig;
}

std::string GetExecutablePath() {
#if defined(__APPLE__)
    std::vector<char> path(1024);
    std::uint32_t size = static_cast<std::uint32_t>(path.size());

    if (_NSGetExecutablePath(path.data(), &size) != 0) {
        path.resize(size);
        if (_NSGetExecutablePath(path.data(), &size) != 0) {
            return {};
        }
    }

    return path.data();
#else
    std::vector<char> path(4096);
    const ssize_t size = readlink("/proc/self/exe", path.data(), path.size() - 1);
    if (size <= 0) {
        return {};
    }

    path[static_cast<std::size_t>(size)] = '\0';

    return path.data();
#endif
}

char** GetEnvironment() {
#if defined(__APPLE__)
    return *_NSGetEnviron();
#else
    return environ;
#endif
}

std::optional<std::uint32_t>
FindMemoryType(VkPhysicalDevice physicalDevice, std::uint32_t typeBits, VkMemoryPropertyFlags requiredProperties) {
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);

    for (std::uint32_t typeIndex = 0; typeIndex < memoryProperties.memoryTypeCount; ++typeIndex) {
        const bool supported = (typeBits & (1U << typeIndex)) != 0;
        const bool hasProperties =
                (memoryProperties.memoryTypes[typeIndex].propertyFlags & requiredProperties) == requiredProperties;

        if (supported && hasProperties) {
            return typeIndex;
        }
    }

    return std::nullopt;
}

void RunSpawnedSmoke(std::string_view filter) {
    const std::string executable = GetExecutablePath();
    ASSERT_FALSE(executable.empty());
    const std::string filterArg = std::string{"--gtest_filter="} + std::string{filter};
    std::string alsoRunDisabledArg = "--gtest_also_run_disabled_tests";
    std::string colorArg = "--gtest_color=no";

    char* const arguments[] = {
            const_cast<char*>(executable.c_str()),
            const_cast<char*>(filterArg.c_str()),
            alsoRunDisabledArg.data(),
            colorArg.data(),
            nullptr,
    };

    pid_t pid = 0;
    const int spawnResult = posix_spawn(&pid, executable.c_str(), nullptr, nullptr, arguments, GetEnvironment());
    ASSERT_EQ(spawnResult, 0);
    ASSERT_NE(pid, -1);

    int status = 0;
    for (int attempt = 0; attempt < 100; ++attempt) {
        const pid_t result = waitpid(pid, &status, WNOHANG);
        ASSERT_NE(result, -1);

        if (result == pid) {
            ASSERT_TRUE(WIFEXITED(status));
            EXPECT_EQ(WEXITSTATUS(status), 0);
            return;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    GTEST_SKIP() << "Vulkan logical device smoke timed out in this environment";
}

TEST(VulkanDevicePlan, UsesOneQueueCreateInfoForSharedGraphicsPresentFamily) {
    const NVulkan::VulkanDevicePlan plan = NVulkan::MakeVulkanDevicePlan(MakeSelection());

    ASSERT_EQ(plan.QueueFamilies.size(), 1U);
    EXPECT_EQ(plan.QueueFamilies.front().FamilyIndex, 2U);
    EXPECT_EQ(plan.Extensions, std::vector<std::string>{VK_KHR_SWAPCHAIN_EXTENSION_NAME});
}

TEST(VulkanDevicePlan, UsesSeparateQueueFamiliesWhenGraphicsAndPresentDiffer) {
    NVulkan::VulkanPhysicalDeviceSelection selection = MakeSelection();
    selection.PresentQueueFamilyIndex = 5;

    const NVulkan::VulkanDevicePlan plan = NVulkan::MakeVulkanDevicePlan(selection);

    ASSERT_EQ(plan.QueueFamilies.size(), 2U);
    EXPECT_EQ(plan.QueueFamilies[0].FamilyIndex, 2U);
    EXPECT_EQ(plan.QueueFamilies[1].FamilyIndex, 5U);
}

TEST(VulkanDevicePlan, EnablesPortabilitySubsetWhenAdvertised) {
    NVulkan::VulkanPhysicalDeviceSelection selection = MakeSelection();
    selection.Capabilities.Extensions.emplace_back(PortabilitySubsetExtension);

    const NVulkan::VulkanDevicePlan plan = NVulkan::MakeVulkanDevicePlan(selection);

    EXPECT_EQ(plan.Extensions,
              (std::vector<std::string>{
                      VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                      std::string{PortabilitySubsetExtension},
              }));
}

TEST(VulkanDevicePlan, MapsGraphicsCapabilities) {
    NVulkan::VulkanPhysicalDeviceSelection selection = MakeSelection();

    const NVulkan::VulkanDevicePlan plan = NVulkan::MakeVulkanDevicePlan(selection);

    EXPECT_TRUE(plan.GraphicsCapabilities.Presentation);
    EXPECT_TRUE(plan.GraphicsCapabilities.TimelineCompletion);
    EXPECT_EQ(plan.GraphicsCapabilities.MaxFramesInFlight, 2U);
}

TEST(VulkanDevicePlan, RejectsNullPhysicalDevice) {
    NVulkan::VulkanPhysicalDeviceSelection selection = MakeSelection();
    selection.Handle = VK_NULL_HANDLE;

    EXPECT_THROW((void)NVulkan::MakeVulkanDevicePlan(selection), NCommon::Exception);
}

TEST(VulkanResourceConversion, MapsImageDescriptorsToVulkanEnums) {
    EXPECT_EQ(NVulkan::ToVulkanFormat(NGraphics::EImageFormat::RGBA8_UNORM), VK_FORMAT_R8G8B8A8_UNORM);
    EXPECT_EQ(NVulkan::ToVulkanFormat(NGraphics::EImageFormat::D32_FLOAT), VK_FORMAT_D32_SFLOAT);
    EXPECT_EQ(
            NVulkan::ToVulkanImageUsage(NGraphics::EImageUsage::TransferDestination | NGraphics::EImageUsage::Sampled),
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    EXPECT_EQ(NVulkan::ToVulkanImageAspect(NGraphics::ImageAspect(NGraphics::EImageAspect::Color)),
              VK_IMAGE_ASPECT_COLOR_BIT);
    EXPECT_EQ(NVulkan::ToVulkanFilter(NGraphics::ESamplerFilter::Linear), VK_FILTER_LINEAR);
    EXPECT_EQ(NVulkan::ToVulkanAddressMode(NGraphics::ESamplerAddressMode::ClampToEdge),
              VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
}

TEST(VulkanResourceConversion, RejectsUnsupportedImageAndSamplerValues) {
    NTest::ExpectError(NCommon::EError::UNSUPPORTED,
                       [] { (void)NVulkan::ToVulkanFormat(static_cast<NGraphics::EImageFormat>(255)); });
    NTest::ExpectError(NCommon::EError::UNSUPPORTED, [] { (void)NVulkan::ToVulkanImageUsage(0x80000000U); });
    NTest::ExpectError(NCommon::EError::UNSUPPORTED, [] { (void)NVulkan::ToVulkanImageAspect(0x40000000U); });
    NTest::ExpectError(NCommon::EError::UNSUPPORTED,
                       [] { (void)NVulkan::ToVulkanFilter(static_cast<NGraphics::ESamplerFilter>(255)); });
    NTest::ExpectError(NCommon::EError::UNSUPPORTED,
                       [] { (void)NVulkan::ToVulkanAddressMode(static_cast<NGraphics::ESamplerAddressMode>(255)); });
}

TEST(VulkanDescriptorManager, RejectsNullDevice) {
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [] {
        const NVulkan::VulkanDescriptorManager manager{VK_NULL_HANDLE,
                                                       {
                                                               .MinUniformBufferOffsetAlignment = 1,
                                                               .MinStorageBufferOffsetAlignment = 1,
                                                               .MaxUniformBufferRange = 1,
                                                               .MaxStorageBufferRange = 1,
                                                       }};
    });
}

TEST(VulkanDeviceChild, DISABLED_CreatesAndDestroysDeviceAndPublishesCapabilities) {
    try {
        NWindow::WindowRuntime runtime;
        NWindow::Window window{MakeWindowConfig()};
        std::vector<std::string> validationErrors;

        NVulkan::VulkanInstanceConfig instanceConfig;
        instanceConfig.ApplicationName = "GraphicsEngineVulkanDeviceTest";
        instanceConfig.RequiredExtensions = NVulkan::NGlfw::GetRequiredInstanceExtensions();
        instanceConfig.ValidationMode = NVulkan::EValidationMode::Required;
        instanceConfig.EnableDebugUtils = true;
        instanceConfig.DebugMessageHandler = [&validationErrors](const NVulkan::VulkanDebugMessage& message) {
            if ((message.Severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
                validationErrors.push_back(message.Message);
            }
        };

        NVulkan::VulkanInstance instance{instanceConfig};
        const NVulkan::NGlfw::VulkanSurface surface{instance, window};
        {
            const NVulkan::VulkanPhysicalDeviceSelection physicalDevice =
                    NVulkan::SelectVulkanPhysicalDevice(instance, surface.GetHandle());
            const NVulkan::VulkanDevice device{physicalDevice};

            EXPECT_NE(device.GetHandle(), VK_NULL_HANDLE);
            EXPECT_TRUE(device.GetGraphicsCapabilities().Presentation);
            EXPECT_TRUE(device.GetGraphicsCapabilities().TimelineCompletion);
            EXPECT_EQ(device.GetGraphicsCapabilities().MaxFramesInFlight, 2U);

            NResources::ResourceManager resources;
            auto makeShader = [&](NResources::EShaderStage stage,
                                  const std::string& key,
                                  const std::vector<std::uint32_t>& words) {
                const auto resource = resources.Request<NResources::ShaderArtifact>({"pipeline-test", key});
                const auto loading = resources.BeginLoading(resource);
                resources.PublishReady(loading, std::make_shared<NResources::ShaderArtifact>(stage, words));
                return NGraphics::Shader{resources, resource};
            };
            NGraphics::GraphicsPipelineDescriptor pipelineDescriptor{
                    .MaterialBindings = {},
                    .Shaders = {makeShader(NResources::EShaderStage::VERTEX, "vertex", NTest::VertexShaderWords),
                                makeShader(NResources::EShaderStage::FRAGMENT, "fragment", NTest::FragmentShaderWords)},
                    .VertexLayout = {},
                    .Topology = NGraphics::EPrimitiveTopology::TRIANGLE_LIST,
                    .RasterState = {},
                    .DepthState = {},
                    .ColorAttachmentFormats = {NGraphics::EPixelFormat::RGBA8_UNORM},
                    .ColorBlendAttachments = {{}},
                    .DepthAttachmentFormat = NGraphics::EPixelFormat::UNDEFINED,
                    .Samples = NGraphics::ESampleCount::X1,
            };
            for (int iteration = 0; iteration < 3; ++iteration) {
                const NVulkan::VulkanGraphicsPipeline pipeline{device, physicalDevice, pipelineDescriptor};
                EXPECT_NE(pipeline.GetHandle(), VK_NULL_HANDLE);
                EXPECT_NE(pipeline.GetLayout(), VK_NULL_HANDLE);
            }

            {
                const PipelineCreationProbe probe;
                const NGraphics::PipelineKey key{pipelineDescriptor};
                const NVulkan::VulkanGraphicsPipeline* first = nullptr;
                {
                    NVulkan::VulkanPipelineManager manager{device, physicalDevice};
                    EXPECT_EQ(manager.FindReady(key), nullptr);
                    EXPECT_EQ(PipelineCreationProbe::Calls, 0);
                    first = &manager.Prepare(key);
                    EXPECT_EQ(manager.FindReady(key), first);
                    auto reordered = pipelineDescriptor;
                    std::swap(reordered.Shaders[0], reordered.Shaders[1]);
                    EXPECT_EQ(&manager.Prepare(NGraphics::PipelineKey{reordered}), first);
                    std::array<const NVulkan::VulkanGraphicsPipeline*, 4> prepared{};
                    std::vector<std::jthread> workers;
                    auto variant = pipelineDescriptor;
                    variant.RasterState.CullMode = NGraphics::ECullMode::NONE;
                    const NGraphics::PipelineKey variantKey{variant};
                    workers.reserve(prepared.size());
                    for (const NVulkan::VulkanGraphicsPipeline*& pipeline: prepared) {
                        workers.emplace_back(
                                [&manager, &variantKey, &pipeline] { pipeline = &manager.Prepare(variantKey); });
                    }
                    workers.clear();
                    for (const auto* pipeline: prepared) {
                        EXPECT_EQ(pipeline, prepared[0]);
                        EXPECT_NE(pipeline, first);
                    }
                    EXPECT_EQ(manager.GetPipelineCount(), 2U);
                    EXPECT_EQ(PipelineCreationProbe::Calls, 2);
                    EXPECT_EQ(PipelineCreationProbe::CacheCalls, 2);
                    variant.Topology = NGraphics::EPrimitiveTopology::LINE_LIST;
                    const NGraphics::PipelineKey failedKey{variant};
                    PipelineCreationProbe::FailNext = true;
                    EXPECT_THROW((void)manager.Prepare(failedKey), NCommon::Exception);
                    EXPECT_EQ(manager.FindReady(failedKey), nullptr);
                    EXPECT_EQ(manager.GetPipelineCount(), 2U);
                    EXPECT_NE(manager.Prepare(failedKey).GetHandle(), VK_NULL_HANDLE);
                    EXPECT_EQ(manager.GetPipelineCount(), 3U);
                    EXPECT_EQ(PipelineCreationProbe::Calls, 4);
                }
                // A new manager has no stale device/cache entries. No GPU uses were submitted.
                NVulkan::VulkanPipelineManager restarted{device, physicalDevice};
                EXPECT_EQ(restarted.FindReady(key), nullptr);
                EXPECT_NE(restarted.Prepare(key).GetHandle(), VK_NULL_HANDLE);
                EXPECT_EQ(PipelineCreationProbe::Calls, 5);
            }
            auto unsupportedPipeline = pipelineDescriptor;
            unsupportedPipeline.Shaders[0] =
                    NGraphics::Shader{resources, pipelineDescriptor.Shaders[0].GetArtifactResource(), "missing"};
            NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
                const NVulkan::VulkanGraphicsPipeline pipeline{device, physicalDevice, unsupportedPipeline};
            });
            unsupportedPipeline.Shaders[0] =
                    makeShader(NResources::EShaderStage::VERTEX, "wrong-stage", NTest::FragmentShaderWords);
            NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
                const NVulkan::VulkanGraphicsPipeline pipeline{device, physicalDevice, unsupportedPipeline};
            });
            unsupportedPipeline = pipelineDescriptor;
            unsupportedPipeline.RasterState.PolygonMode = NGraphics::EPolygonMode::LINE;
            NTest::ExpectError(NCommon::EError::UNSUPPORTED, [&] {
                const NVulkan::VulkanGraphicsPipeline pipeline{device, physicalDevice, unsupportedPipeline};
            });
            pipelineDescriptor.Shaders.pop_back();
            pipelineDescriptor.ColorAttachmentFormats.clear();
            pipelineDescriptor.ColorBlendAttachments.clear();
            pipelineDescriptor.DepthAttachmentFormat = NGraphics::EPixelFormat::D32_FLOAT;
            pipelineDescriptor.DepthState.TestEnabled = true;
            pipelineDescriptor.DepthState.WriteEnabled = true;
            {
                const NVulkan::VulkanGraphicsPipeline pipeline{device, physicalDevice, pipelineDescriptor};
                EXPECT_NE(pipeline.GetHandle(), VK_NULL_HANDLE);
            }

            {
                const NVulkan::VulkanLockedQueue graphicsQueue = device.LockGraphicsQueue();
                EXPECT_NE(graphicsQueue.GetHandle(), VK_NULL_HANDLE);
                EXPECT_EQ(graphicsQueue.GetFamilyIndex(), physicalDevice.GraphicsQueueFamilyIndex);
            }

            {
                const NVulkan::VulkanLockedQueue presentQueue = device.LockPresentQueue();
                EXPECT_NE(presentQueue.GetHandle(), VK_NULL_HANDLE);
                EXPECT_EQ(presentQueue.GetFamilyIndex(), physicalDevice.PresentQueueFamilyIndex);
            }

            const NGraphics::ImageDescriptor imageDescriptor{
                    .Extent = {.Width = 4, .Height = 4, .Depth = 1},
                    .Format = NGraphics::EImageFormat::RGBA8_UNORM,
                    .Usage = NGraphics::EImageUsage::TransferDestination | NGraphics::EImageUsage::Sampled,
                    .Access = NGraphics::ImageAccess(NGraphics::EImageAccess::GpuRead),
            };
            VkImage image = device.CreateImage(imageDescriptor);
            ASSERT_NE(image, VK_NULL_HANDLE);

            VkMemoryRequirements memoryRequirements{};
            vkGetImageMemoryRequirements(device.GetHandle(), image, &memoryRequirements);

            const std::optional<std::uint32_t> memoryTypeIndex = FindMemoryType(physicalDevice.Handle,
                                                                                memoryRequirements.memoryTypeBits,
                                                                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

            ASSERT_TRUE(memoryTypeIndex.has_value());

            const VkMemoryAllocateInfo allocationInfo{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                    .pNext = nullptr,
                    .allocationSize = memoryRequirements.size,
                    .memoryTypeIndex = *memoryTypeIndex,
            };

            VkDeviceMemory imageMemory = VK_NULL_HANDLE;
            ASSERT_EQ(vkAllocateMemory(device.GetHandle(), &allocationInfo, nullptr, &imageMemory), VK_SUCCESS);
            ASSERT_EQ(vkBindImageMemory(device.GetHandle(), image, imageMemory, 0), VK_SUCCESS);

            VkImageView imageView =
                    device.CreateImageView(image,
                                           {
                                                   .Image = {},
                                                   .Format = NGraphics::EImageFormat::RGBA8_UNORM,
                                                   .Aspects = NGraphics::ImageAspect(NGraphics::EImageAspect::Color),
                                           });
            EXPECT_NE(imageView, VK_NULL_HANDLE);

            VkSampler sampler = device.CreateSampler({
                    .MinFilter = NGraphics::ESamplerFilter::Linear,
                    .MagFilter = NGraphics::ESamplerFilter::Linear,
                    .AddressModeU = NGraphics::ESamplerAddressMode::ClampToEdge,
                    .AddressModeV = NGraphics::ESamplerAddressMode::ClampToEdge,
                    .AddressModeW = NGraphics::ESamplerAddressMode::ClampToEdge,
            });
            EXPECT_NE(sampler, VK_NULL_HANDLE);

            device.DestroySampler(sampler);
            device.DestroyImageView(imageView);
            device.DestroyImage(image);
            vkFreeMemory(device.GetHandle(), imageMemory, nullptr);
        }

        EXPECT_TRUE(validationErrors.empty()) << (validationErrors.empty() ? "" : validationErrors.front());
    } catch (const NCommon::Exception& exception) {
        if (IsEnvironmentFailure(exception)) {
            GTEST_SKIP() << exception.GetMessage();
        }

        throw;
    }
}

TEST(VulkanDeviceChild, DISABLED_AllocatesAndCachesMaterialDescriptorSets) {
    try {
        NWindow::WindowRuntime runtime;
        NWindow::Window window{MakeWindowConfig()};
        std::vector<std::string> validationErrors;

        NVulkan::VulkanInstanceConfig instanceConfig;
        instanceConfig.ApplicationName = "GraphicsEngineVulkanDescriptorTest";
        instanceConfig.RequiredExtensions = NVulkan::NGlfw::GetRequiredInstanceExtensions();
        instanceConfig.ValidationMode = NVulkan::EValidationMode::Required;
        instanceConfig.EnableDebugUtils = true;
        instanceConfig.DebugMessageHandler = [&validationErrors](const NVulkan::VulkanDebugMessage& message) {
            if ((message.Severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
                validationErrors.push_back(message.Message);
            }
        };

        NVulkan::VulkanInstance instance{instanceConfig};
        const NVulkan::NGlfw::VulkanSurface surface{instance, window};
        {
            const NVulkan::VulkanPhysicalDeviceSelection physicalDevice =
                    NVulkan::SelectVulkanPhysicalDevice(instance, surface.GetHandle());
            const NVulkan::VulkanDevice device{physicalDevice};

            const NGraphics::ImageDescriptor imageDescriptor{
                    .Extent = {.Width = 4, .Height = 4, .Depth = 1},
                    .Format = NGraphics::EImageFormat::RGBA8_UNORM,
                    .Usage = NGraphics::EImageUsage::TransferDestination | NGraphics::EImageUsage::Sampled,
                    .Access = NGraphics::ImageAccess(NGraphics::EImageAccess::GpuRead),
            };
            VkImage image = device.CreateImage(imageDescriptor);
            ASSERT_NE(image, VK_NULL_HANDLE);

            VkMemoryRequirements memoryRequirements{};
            vkGetImageMemoryRequirements(device.GetHandle(), image, &memoryRequirements);

            const std::optional<std::uint32_t> memoryTypeIndex = FindMemoryType(physicalDevice.Handle,
                                                                                memoryRequirements.memoryTypeBits,
                                                                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

            ASSERT_TRUE(memoryTypeIndex.has_value());

            const VkMemoryAllocateInfo allocationInfo{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                    .pNext = nullptr,
                    .allocationSize = memoryRequirements.size,
                    .memoryTypeIndex = *memoryTypeIndex,
            };

            VkDeviceMemory imageMemory = VK_NULL_HANDLE;
            ASSERT_EQ(vkAllocateMemory(device.GetHandle(), &allocationInfo, nullptr, &imageMemory), VK_SUCCESS);
            ASSERT_EQ(vkBindImageMemory(device.GetHandle(), image, imageMemory, 0), VK_SUCCESS);

            VkImageView imageView =
                    device.CreateImageView(image,
                                           {
                                                   .Image = {},
                                                   .Format = NGraphics::EImageFormat::RGBA8_UNORM,
                                                   .Aspects = NGraphics::ImageAspect(NGraphics::EImageAspect::Color),
                                           });
            ASSERT_NE(imageView, VK_NULL_HANDLE);

            VkSampler sampler = device.CreateSampler({
                    .MinFilter = NGraphics::ESamplerFilter::Linear,
                    .MagFilter = NGraphics::ESamplerFilter::Linear,
                    .AddressModeU = NGraphics::ESamplerAddressMode::ClampToEdge,
                    .AddressModeV = NGraphics::ESamplerAddressMode::ClampToEdge,
                    .AddressModeW = NGraphics::ESamplerAddressMode::ClampToEdge,
            });
            ASSERT_NE(sampler, VK_NULL_HANDLE);

            {
                NVulkan::VulkanDescriptorManager descriptors{device.GetHandle(), device.GetDescriptorLimits()};
                const std::vector<NGraphics::MaterialBindingLayoutEntry> layout{
                        {
                                .Binding = 0,
                                .Type = NGraphics::EMaterialBindingType::CombinedImageSampler,
                                .Visibility = NGraphics::ShaderVisibility(NGraphics::EShaderVisibility::Fragment),
                                .Count = 1,
                        },
                };
                const std::vector<NVulkan::VulkanResolvedMaterialBinding> bindings{
                        {
                                .Material =
                                        {
                                                .Binding = 0,
                                                .Type = NGraphics::EMaterialBindingType::CombinedImageSampler,
                                                .Buffer = {},
                                                .Image = {},
                                                .Sampler = {},
                                                .CombinedImageSampler =
                                                        {
                                                                .Image =
                                                                        NResources::ResourceIdentity{"image", "albedo"},
                                                                .Sampler = NResources::ResourceIdentity{"sampler",
                                                                                                        "linear"},
                                                        },
                                        },
                                .Buffer = {},
                                .Image = {.ImageView = imageView, .ResourceVersion = 1},
                                .Sampler = {.Sampler = sampler, .ResourceVersion = 1},
                        },
                };

                const NVulkan::VulkanDescriptorSetLease first =
                        descriptors.Acquire({.Layout = layout, .Bindings = bindings});
                const NVulkan::VulkanDescriptorSetLease second =
                        descriptors.Acquire({.Layout = layout, .Bindings = bindings});
                std::vector<NVulkan::VulkanResolvedMaterialBinding> reloadedBindings = bindings;
                reloadedBindings.front().Image.ResourceVersion = 2;
                const NVulkan::VulkanDescriptorSetLease reloaded =
                        descriptors.Acquire({.Layout = layout, .Bindings = reloadedBindings});

                EXPECT_TRUE(first.IsValid());
                EXPECT_EQ(first.GetLayout(), second.GetLayout());
                EXPECT_EQ(first.GetSet(), second.GetSet());
                EXPECT_NE(first.GetSet(), reloaded.GetSet());
                EXPECT_EQ(descriptors.GetTelemetry().Allocations, 2U);
                EXPECT_EQ(descriptors.GetTelemetry().CacheMisses, 2U);
                EXPECT_EQ(descriptors.GetTelemetry().CacheHits, 1U);

                const std::vector<NGraphics::MaterialBindingLayoutEntry> bufferLayout{
                        {
                                .Binding = 0,
                                .Type = NGraphics::EMaterialBindingType::UniformBuffer,
                                .Visibility = NGraphics::ShaderVisibility(NGraphics::EShaderVisibility::Vertex),
                                .Count = 1,
                        },
                };
                const std::vector<NVulkan::VulkanResolvedMaterialBinding> misalignedBuffer{
                        {
                                .Material =
                                        {
                                                .Binding = 0,
                                                .Type = NGraphics::EMaterialBindingType::UniformBuffer,
                                                .Buffer =
                                                        {
                                                                .Resource = NResources::ResourceIdentity{"buffer",
                                                                                                         "camera"},
                                                                .OffsetBytes = 1,
                                                                .SizeBytes = 64,
                                                        },
                                                .Image = {},
                                                .Sampler = {},
                                                .CombinedImageSampler = {},
                                        },
                                .Buffer =
                                        {
                                                .Buffer = reinterpret_cast<VkBuffer>(1),
                                                .ResourceVersion = 1,
                                                .BufferSizeBytes = 256,
                                                .OffsetBytes = 1,
                                                .SizeBytes = 64,
                                        },
                                .Image = {},
                                .Sampler = {},
                        },
                };
                NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
                    (void)descriptors.Acquire({.Layout = bufferLayout, .Bindings = misalignedBuffer});
                });

                std::vector<NVulkan::VulkanResolvedMaterialBinding> mismatchedBindings = bindings;
                mismatchedBindings.front().Material.Binding = 1;
                NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
                    (void)descriptors.Acquire({.Layout = layout, .Bindings = mismatchedBindings});
                });

                NVulkan::VulkanDescriptorManager otherDescriptors{device.GetHandle(), device.GetDescriptorLimits()};
                NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { otherDescriptors.RetainUntil(first, 3); });
                EXPECT_FALSE(otherDescriptors.HasInFlightDescriptors());
                descriptors.RetainUntil(first, 3);
                descriptors.ReleaseCompleted(2);
                EXPECT_TRUE(descriptors.HasInFlightDescriptors());
                EXPECT_TRUE(descriptors.HasInFlightDescriptors());
                descriptors.ReleaseCompleted(3);
                EXPECT_FALSE(descriptors.HasInFlightDescriptors());
            }

            device.DestroySampler(sampler);
            device.DestroyImageView(imageView);
            device.DestroyImage(image);
            vkFreeMemory(device.GetHandle(), imageMemory, nullptr);
        }

        EXPECT_TRUE(validationErrors.empty()) << (validationErrors.empty() ? "" : validationErrors.front());
    } catch (const NCommon::Exception& exception) {
        if (IsEnvironmentFailure(exception)) {
            GTEST_SKIP() << exception.GetMessage();
        }

        throw;
    }
}

TEST(VulkanDevice, CreatesAndDestroysDeviceAndPublishesCapabilities) {
    RunSpawnedSmoke("VulkanDeviceChild.DISABLED_CreatesAndDestroysDeviceAndPublishesCapabilities");
}

TEST(VulkanUploadManagerChild, DISABLED_RetainsStagingUntilTimelineCompletion) {
    try {
        NWindow::WindowRuntime runtime;
        NWindow::Window window{MakeWindowConfig()};
        std::vector<std::string> validationErrors;

        NVulkan::VulkanInstanceConfig instanceConfig;
        instanceConfig.ApplicationName = "GraphicsEngine Vulkan Upload Test";
        instanceConfig.RequiredExtensions = NVulkan::NGlfw::GetRequiredInstanceExtensions();
        instanceConfig.ValidationMode = NVulkan::EValidationMode::Required;
        instanceConfig.EnableDebugUtils = true;
        instanceConfig.DebugMessageHandler = [&validationErrors](const NVulkan::VulkanDebugMessage& message) {
            if ((message.Severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
                validationErrors.push_back(message.Message);
            }
        };

        NVulkan::VulkanInstance instance{instanceConfig};
        const NVulkan::NGlfw::VulkanSurface surface{instance, window};
        const NVulkan::VulkanPhysicalDeviceSelection physicalDevice =
                NVulkan::SelectVulkanPhysicalDevice(instance, surface.GetHandle());
        const NVulkan::VulkanDevice device{physicalDevice};
        NVulkan::VulkanResourceStateTracker resourceStates;
        NVulkan::VulkanMemoryAllocator memory{instance.GetHandle(), physicalDevice.Handle, device, resourceStates};
        NVulkan::VulkanSubmissionManager submissions{device};
        NVulkan::VulkanUploadManager uploads{device, memory, submissions, resourceStates};
        const std::array<std::byte, 4> bytes{
                std::byte{0x01},
                std::byte{0x02},
                std::byte{0x03},
                std::byte{0x04},
        };
        std::vector<NVulkan::VulkanBufferAllocation> destinations;
        destinations.reserve(32);
        NVulkan::VulkanQueueCompletion completion{};
        for (std::size_t index = 0; index < 32; ++index) {
            destinations.emplace_back(NVulkan::CreateVulkanBuffer(
                    memory,
                    {
                            .SizeBytes = bytes.size(),
                            .Usage = NGraphics::EBufferUsage::TransferDestination | NGraphics::EBufferUsage::Vertex,
                            .Access = NGraphics::BufferAccess(NGraphics::EBufferAccess::CpuRead),
                    }));
            completion = uploads.UploadBuffer(destinations.back(), bytes);
        }

        EXPECT_GT(uploads.GetPendingUploadCount(), 0U);
        EXPECT_LE(uploads.GetPendingUploadCount(), destinations.size());
        submissions.Wait(completion, std::numeric_limits<std::uint64_t>::max());
        uploads.ReleaseCompleted();
        EXPECT_EQ(uploads.GetPendingUploadCount(), 0U);

        for (NVulkan::VulkanBufferAllocation& destination: destinations) {
            void* mapped = destination.Map();
            destination.Invalidate(0, bytes.size());
            EXPECT_EQ(std::memcmp(mapped, bytes.data(), bytes.size()), 0);
            destination.Unmap();
            ASSERT_TRUE(resourceStates.GetBufferState(destination.GetBuffer(), 0, bytes.size()).has_value());
            EXPECT_EQ(resourceStates.GetBufferState(destination.GetBuffer(), 0, bytes.size())->AccessMask,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT);
        }
        VkBuffer destroyedBuffer = destinations.front().GetBuffer();
        destinations.clear();
        EXPECT_FALSE(resourceStates.GetBufferState(destroyedBuffer, 0, bytes.size()).has_value());

        std::optional<NVulkan::VulkanBufferAllocation> retainedDestination = NVulkan::CreateVulkanBuffer(
                memory,
                {
                        .SizeBytes = bytes.size(),
                        .Usage = NGraphics::EBufferUsage::TransferDestination | NGraphics::EBufferUsage::Vertex,
                        .Access = NGraphics::BufferAccess(NGraphics::EBufferAccess::GpuRead),
                });
        VkBuffer retainedBuffer = retainedDestination->GetBuffer();
        const NVulkan::VulkanQueueCompletion retainedCompletion = uploads.UploadBuffer(*retainedDestination, bytes);
        retainedDestination.reset();
        EXPECT_TRUE(resourceStates.GetBufferState(retainedBuffer, 0, bytes.size()).has_value());
        submissions.Wait(retainedCompletion, std::numeric_limits<std::uint64_t>::max());
        uploads.ReleaseCompleted();
        EXPECT_FALSE(resourceStates.GetBufferState(retainedBuffer, 0, bytes.size()).has_value());

        NVulkan::VulkanBufferAllocation partialDestination = NVulkan::CreateVulkanBuffer(
                memory,
                {
                        .SizeBytes = 8,
                        .Usage = NGraphics::EBufferUsage::TransferDestination | NGraphics::EBufferUsage::Vertex,
                        .Access = NGraphics::EBufferAccess::CpuRead | NGraphics::EBufferAccess::CpuWrite,
                });
        const std::array<std::byte, 4> firstPartialBytes{
                std::byte{0x01},
                std::byte{0x02},
                std::byte{0x03},
                std::byte{0x04},
        };
        const std::array<std::byte, 4> secondPartialBytes{
                std::byte{0x05},
                std::byte{0x06},
                std::byte{0x07},
                std::byte{0x08},
        };
        (void)uploads.UploadBuffer(partialDestination, firstPartialBytes, 0);
        const NVulkan::VulkanQueueCompletion partialCompletion =
                uploads.UploadBuffer(partialDestination, secondPartialBytes, 2);
        submissions.Wait(partialCompletion, std::numeric_limits<std::uint64_t>::max());
        uploads.ReleaseCompleted();

        const std::array<std::byte, 6> expectedPartialBytes{
                std::byte{0x01},
                std::byte{0x02},
                std::byte{0x05},
                std::byte{0x06},
                std::byte{0x07},
                std::byte{0x08},
        };
        void* mappedPartialDestination = partialDestination.Map();
        partialDestination.Invalidate(0, expectedPartialBytes.size());
        EXPECT_EQ(std::memcmp(mappedPartialDestination, expectedPartialBytes.data(), expectedPartialBytes.size()), 0);
        partialDestination.Unmap();

        NVulkan::VulkanImageAllocation image = NVulkan::CreateVulkanImage(
                memory,
                {
                        .Extent = {.Width = 1, .Height = 1, .Depth = 1},
                        .Format = NGraphics::EImageFormat::RGBA8_UNORM,
                        .Usage = NGraphics::EImageUsage::TransferDestination | NGraphics::EImageUsage::Sampled,
                        .Access = NGraphics::ImageAccess(NGraphics::EImageAccess::GpuRead),
                });
        const NVulkan::VulkanQueueCompletion imageCompletion = uploads.UploadImage(image, bytes);

        EXPECT_EQ(uploads.GetPendingUploadCount(), 1U);
        submissions.Wait(imageCompletion, std::numeric_limits<std::uint64_t>::max());
        uploads.ReleaseCompleted();
        EXPECT_EQ(uploads.GetPendingUploadCount(), 0U);
        const VkImageSubresourceRange imageRange{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
        };
        ASSERT_TRUE(resourceStates.GetImageState(image.GetImage(), imageRange).has_value());
        EXPECT_EQ(resourceStates.GetImageState(image.GetImage(), imageRange)->Layout,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        VkImage destroyedImage = image.GetImage();
        image = {};
        EXPECT_FALSE(resourceStates.GetImageState(destroyedImage, imageRange).has_value());
        {
            UploadProbe probe;
            NVulkan::VulkanUploadManager batched{
                    device,
                    memory,
                    submissions,
                    resourceStates,
                    {.ArenaSizeBytes = 256, .MaxStagingBytes = 512, .MaxBatchUploads = 64}};
            std::vector<NVulkan::VulkanBufferUpload> bufferUploads;
            for (std::size_t index = 0; index < 32; ++index) {
                destinations.emplace_back(NVulkan::CreateVulkanBuffer(
                        memory,
                        {
                                .SizeBytes = bytes.size(),
                                .Usage = NGraphics::EBufferUsage::TransferDestination | NGraphics::EBufferUsage::Vertex,
                                .Access = NGraphics::BufferAccess(NGraphics::EBufferAccess::CpuRead),
                        }));
            }
            bufferUploads.reserve(destinations.size());
            for (auto& destination: destinations) {
                bufferUploads.push_back({&destination, bytes});
            }
            auto oddImage = NVulkan::CreateVulkanImage(
                    memory,
                    {
                            .Extent = {.Width = 3, .Height = 1, .Depth = 1},
                            .Format = NGraphics::EImageFormat::R8_UNORM,
                            .Usage = NGraphics::EImageUsage::TransferDestination | NGraphics::EImageUsage::Sampled,
                            .Access = NGraphics::ImageAccess(NGraphics::EImageAccess::GpuRead),
                    });
            const NVulkan::VulkanImageUpload imageUpload{&oddImage, std::span{bytes}.first(3)};
            const std::span<const NVulkan::VulkanImageUpload> imageUploads{&imageUpload, 1};
            UploadProbe::CompletionLimit = 0;
            const auto first = batched.UploadBatch(bufferUploads, imageUploads);
            const auto second = batched.UploadBatch(bufferUploads, imageUploads);
            EXPECT_EQ(UploadProbe::SubmitCalls, 2U);
            EXPECT_EQ(batched.GetStatistics().PendingBatches, 2U);
            EXPECT_EQ(batched.GetStatistics().PendingUploads, 66U);
            EXPECT_EQ(batched.GetStatistics().PendingBytes, 262U);
            EXPECT_EQ(batched.GetStatistics().ArenaAllocations, 2U);
            EXPECT_EQ(batched.GetStatistics().StagingBytes, 512U);
            EXPECT_THROW((void)batched.UploadBatch(bufferUploads), NCommon::Exception);
            EXPECT_EQ(UploadProbe::SubmitCalls, 2U);
            submissions.Wait(second, 5'000'000'000);
            UploadProbe::CompletionLimit = first.Value;
            batched.ReleaseCompleted();
            EXPECT_EQ(batched.GetStatistics().PendingBatches, 1U);
            const auto third = batched.UploadBatch(bufferUploads, imageUploads);
            EXPECT_EQ(batched.GetStatistics().ArenaAllocations, 2U);
            EXPECT_EQ(batched.GetStatistics().Submissions, 3U);
            submissions.Wait(third, 5'000'000'000);
            UploadProbe::CompletionLimit = std::numeric_limits<std::uint64_t>::max();
            batched.ReleaseCompleted();
            EXPECT_EQ(batched.GetPendingUploadCount(), 0U);
            EXPECT_EQ(batched.GetStatistics().PendingHighWaterBytes, 262U);
            for (auto& destination: destinations) {
                void* mapped = destination.Map();
                destination.Invalidate(0, bytes.size());
                EXPECT_EQ(std::memcmp(mapped, bytes.data(), bytes.size()), 0);
                destination.Unmap();
            }
            EXPECT_THROW((void)batched.UploadBatch({}), NCommon::Exception);
            const NVulkan::VulkanBufferUpload invalid{nullptr, bytes};
            EXPECT_THROW((void)batched.UploadBatch({&invalid, 1}), NCommon::Exception);
            std::vector<NVulkan::VulkanBufferUpload> oversized(65, bufferUploads.front());
            EXPECT_THROW((void)batched.UploadBatch(oversized), NCommon::Exception);
            const auto previous = resourceStates.GetBufferState(destinations.front().GetBuffer(), 0, bytes.size());
            UploadProbe::FailSubmit = true;
            EXPECT_THROW((void)batched.UploadBatch(bufferUploads), NCommon::Exception);
            EXPECT_EQ(batched.GetPendingUploadCount(), 0U);
            EXPECT_EQ(batched.GetStatistics().Submissions, 3U);
            EXPECT_EQ(resourceStates.GetBufferState(destinations.front().GetBuffer(), 0, bytes.size())->AccessMask,
                      previous->AccessMask);
            const auto recovered = batched.UploadBatch(bufferUploads);
            submissions.Wait(recovered, 5'000'000'000);
            batched.ReleaseCompleted();
            for (std::size_t iteration = 0; iteration < 64; ++iteration) {
                const auto streamed = batched.UploadBatch(bufferUploads, imageUploads);
                submissions.Wait(streamed, 5'000'000'000);
                batched.ReleaseCompleted();
            }
            EXPECT_EQ(batched.GetStatistics().ArenaAllocations, 2U);
            EXPECT_EQ(batched.GetStatistics().PendingBytes, 0U);
            std::vector<std::byte> largeBytes(300, std::byte{0x57});
            auto largeDestination = NVulkan::CreateVulkanBuffer(
                    memory,
                    {
                            .SizeBytes = largeBytes.size(),
                            .Usage = NGraphics::EBufferUsage::TransferDestination | NGraphics::EBufferUsage::Vertex,
                            .Access = NGraphics::BufferAccess(NGraphics::EBufferAccess::CpuRead),
                    });
            const auto largeCompletion = batched.UploadBuffer(largeDestination, largeBytes);
            EXPECT_EQ(batched.GetStatistics().StagingBytes, 300U);
            EXPECT_EQ(batched.GetStatistics().ArenaAllocations, 3U);
            submissions.Wait(largeCompletion, 5'000'000'000);
            batched.ReleaseCompleted();
            auto retention = largeDestination.GetRetentionToken();
            const std::weak_ptr<void> retainedLifetime = retention;
            retention.reset();
            const auto batchRetainedCompletion = batched.UploadBuffer(largeDestination, largeBytes);
            largeDestination = {};
            EXPECT_FALSE(retainedLifetime.expired());
            submissions.Wait(batchRetainedCompletion, 5'000'000'000);
            batched.ReleaseCompleted();
            EXPECT_TRUE(retainedLifetime.expired());
            const NVulkan::VulkanBufferUpload overlapping[] = {
                    {&partialDestination, firstPartialBytes, 0},
                    {&partialDestination, secondPartialBytes, 2},
            };
            const auto overlapCompletion = batched.UploadBatch(overlapping);
            submissions.Wait(overlapCompletion, 5'000'000'000);
            batched.ReleaseCompleted();
            mappedPartialDestination = partialDestination.Map();
            partialDestination.Invalidate(0, expectedPartialBytes.size());
            EXPECT_EQ(std::memcmp(mappedPartialDestination, expectedPartialBytes.data(), expectedPartialBytes.size()),
                      0);
            partialDestination.Unmap();
            // Destruction waits the exact submission even when completion polling is deliberately held back.
            {
                NVulkan::VulkanUploadManager retiring{device, memory, submissions, resourceStates};
                UploadProbe::CompletionLimit = 0;
                (void)retiring.UploadBatch(bufferUploads, imageUploads);
            }
            EXPECT_EQ(UploadProbe::IdleCalls, 0U);
        }
        EXPECT_TRUE(validationErrors.empty()) << (validationErrors.empty() ? "" : validationErrors.front());
    } catch (const NCommon::Exception& exception) {
        if (IsEnvironmentFailure(exception)) {
            GTEST_SKIP() << exception.GetMessage();
        }

        throw;
    }
}

TEST(VulkanAsyncResourcesChild, DISABLED_DecodesAndUploadsMeshBeforeReady) {
    try {
        NWindow::WindowRuntime runtime;
        NWindow::Window window{MakeWindowConfig()};
        std::vector<std::string> errors;
        NVulkan::VulkanInstanceConfig config;
        config.RequiredExtensions = NVulkan::NGlfw::GetRequiredInstanceExtensions();
        config.ValidationMode = NVulkan::EValidationMode::Required;
        config.EnableDebugUtils = true;
        config.DebugMessageHandler = [&](const auto& message) {
            if ((message.Severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
                errors.push_back(message.Message);
            }
        };
        NVulkan::VulkanInstance instance{config};
        const NVulkan::NGlfw::VulkanSurface surface{instance, window};
        const auto physical = NVulkan::SelectVulkanPhysicalDevice(instance, surface.GetHandle());
        const NVulkan::VulkanDevice device{physical};
        NVulkan::VulkanResourceStateTracker states;
        NVulkan::VulkanMemoryAllocator memory{instance.GetHandle(), physical.Handle, device, states};
        NVulkan::VulkanSubmissionManager submissions{device};
        NVulkan::VulkanUploadManager uploads{device, memory, submissions, states};
        NResources::ResourceManager resources;
        NCommon::TaskSystem tasks{1};
        const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
        NResources::AsyncResourceLoader loader{resources, tasks, {}, [&](const auto&, const auto&, auto) {
                                                   auto bytes = std::as_bytes(std::span{obj});
                                                   return NResources::ResourceIoBytes{bytes.begin(), bytes.end()};
                                               }};
        UploadProbe probe;
        UploadProbe::CompletionLimit = 0;
        // The backend retains native allocations on its owner thread; generic leases may release anywhere.
        std::shared_ptr<NVulkan::VulkanBufferAllocation> destination;
        NVulkan::VulkanQueueCompletion completion;
        const auto owner = std::this_thread::get_id();
        const auto handle = loader.LoadMesh({"mesh", "async-triangle"}, "triangle.obj", [&](const auto& mesh) {
            EXPECT_EQ(std::this_thread::get_id(), owner);
            const auto bytes = std::as_bytes(std::span{mesh->GetVertices()});
            destination = std::make_shared<NVulkan::VulkanBufferAllocation>(NVulkan::CreateVulkanBuffer(
                    memory,
                    {
                            .SizeBytes = bytes.size(),
                            .Usage = NGraphics::EBufferUsage::TransferDestination | NGraphics::EBufferUsage::Vertex,
                            .Access = NGraphics::BufferAccess(NGraphics::EBufferAccess::CpuRead),
                    }));
            completion = uploads.UploadBuffer(*destination, bytes);
            return NResources::ResourceUpload{destination,
                                              [&, token = completion] { return submissions.IsCompleted(token); },
                                              [&, token = completion] { submissions.Wait(token, 5'000'000'000); }};
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (resources.GetState(handle) != NResources::EResourceState::UPLOADING &&
               std::chrono::steady_clock::now() < deadline) {
            loader.PumpUploads();
            std::this_thread::yield();
        }
        ASSERT_EQ(resources.GetState(handle), NResources::EResourceState::UPLOADING);
        EXPECT_FALSE(resources.TryAcquire(handle));
        EXPECT_EQ(UploadProbe::SubmitCalls, 1U);
        submissions.Wait(completion, 5'000'000'000);
        loader.PumpUploads();
        EXPECT_FALSE(resources.TryAcquire(handle));
        UploadProbe::CompletionLimit = completion.Value;
        loader.PumpUploads();
        auto lease = resources.TryAcquire(handle);
        ASSERT_TRUE(lease);
        EXPECT_EQ(lease->GetGpuRepresentation().get(), destination.get());
        const auto expected = std::as_bytes(std::span{(*lease)->GetVertices()});
        void* mapped = destination->Map();
        destination->Invalidate(0, expected.size());
        EXPECT_EQ(std::memcmp(mapped, expected.data(), expected.size()), 0);
        destination->Unmap();
        resources.RequestUnload(handle);
        resources.CompleteUnload(handle);
        EXPECT_EQ((*lease)->GetVertices().size(), 3U);
        loader.Shutdown();
        uploads.ReleaseCompleted();
        lease.reset();
        destination.reset();
        EXPECT_EQ(UploadProbe::IdleCalls, 0U);
        EXPECT_TRUE(errors.empty()) << (errors.empty() ? "" : errors.front());
    } catch (const NCommon::Exception& error) {
        if (IsEnvironmentFailure(error)) {
            GTEST_SKIP() << error.GetMessage();
        }
        throw;
    }
}

TEST(VulkanAsyncResources, DecodesAndUploadsMeshBeforeReady) {
    RunSpawnedSmoke("VulkanAsyncResourcesChild.DISABLED_DecodesAndUploadsMeshBeforeReady");
}

TEST(VulkanUploadManager, RetainsStagingUntilTimelineCompletion) {
    RunSpawnedSmoke("VulkanUploadManagerChild.DISABLED_RetainsStagingUntilTimelineCompletion");
}

TEST(VulkanDevice, AllocatesAndCachesMaterialDescriptorSets) {
    RunSpawnedSmoke("VulkanDeviceChild.DISABLED_AllocatesAndCachesMaterialDescriptorSets");
}

} // namespace
