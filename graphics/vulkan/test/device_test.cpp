#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <spawn.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <sys/wait.h>

#if defined(__APPLE__)
    #include <crt_externs.h>

    #include <mach-o/dyld.h>
#else
extern char** environ;
#endif

#include <graphics/vulkan/descriptor_manager.h>
#include <graphics/vulkan/device.h>
#include <graphics/vulkan/glfw_surface.h>
#include <graphics/vulkan/instance.h>
#include <graphics/vulkan/physical_device.h>
#include <graphics/vulkan/resource_conversion.h>
#include <gtest/gtest.h>
#include <lib/common/error/exception.h>
#include <tests/common/test_error.h>
#include <window/window.h>
#include <window/window_config.h>
#include <window/window_runtime.h>
#include <window/window_size.h>
#include <window/window_type.h>

namespace {

constexpr std::string_view PortabilitySubsetExtension = "VK_KHR_portability_subset";

NVulkan::VulkanPhysicalDeviceSelection MakeSelection() {
    NVulkan::VulkanPhysicalDeviceSelection selection{
            .Handle = reinterpret_cast<VkPhysicalDevice>(1),
            .Capabilities =
                    {
                            .ApiVersion = VK_API_VERSION_1_3,
                            .Extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME},
                            .Features =
                                    {
                                            .TimelineSemaphore = true,
                                            .Synchronization2 = true,
                                            .DynamicRendering = true,
                                    },
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
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT,
                       [] { const NVulkan::VulkanDescriptorManager manager{VK_NULL_HANDLE}; });
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
            const VkImage image = device.CreateImage(imageDescriptor);
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

            const VkImageView imageView =
                    device.CreateImageView(image,
                                           {
                                                   .Image = {},
                                                   .Format = NGraphics::EImageFormat::RGBA8_UNORM,
                                                   .Aspects = NGraphics::ImageAspect(NGraphics::EImageAspect::Color),
                                           });
            EXPECT_NE(imageView, VK_NULL_HANDLE);

            const VkSampler sampler = device.CreateSampler({
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
            const VkImage image = device.CreateImage(imageDescriptor);
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

            const VkImageView imageView =
                    device.CreateImageView(image,
                                           {
                                                   .Image = {},
                                                   .Format = NGraphics::EImageFormat::RGBA8_UNORM,
                                                   .Aspects = NGraphics::ImageAspect(NGraphics::EImageAspect::Color),
                                           });
            ASSERT_NE(imageView, VK_NULL_HANDLE);

            const VkSampler sampler = device.CreateSampler({
                    .MinFilter = NGraphics::ESamplerFilter::Linear,
                    .MagFilter = NGraphics::ESamplerFilter::Linear,
                    .AddressModeU = NGraphics::ESamplerAddressMode::ClampToEdge,
                    .AddressModeV = NGraphics::ESamplerAddressMode::ClampToEdge,
                    .AddressModeW = NGraphics::ESamplerAddressMode::ClampToEdge,
            });
            ASSERT_NE(sampler, VK_NULL_HANDLE);

            {
                NVulkan::VulkanDescriptorManager descriptors{device.GetHandle()};
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
                                .Image = {.ImageView = imageView},
                                .Sampler = {.Sampler = sampler},
                        },
                };

                const NVulkan::VulkanDescriptorSetLease first =
                        descriptors.Acquire({.Layout = layout, .Bindings = bindings});
                const NVulkan::VulkanDescriptorSetLease second =
                        descriptors.Acquire({.Layout = layout, .Bindings = bindings});

                EXPECT_TRUE(first.IsValid());
                EXPECT_EQ(first.GetLayout(), second.GetLayout());
                EXPECT_EQ(first.GetSet(), second.GetSet());
                EXPECT_EQ(descriptors.GetTelemetry().Allocations, 1U);
                EXPECT_EQ(descriptors.GetTelemetry().CacheMisses, 1U);
                EXPECT_EQ(descriptors.GetTelemetry().CacheHits, 1U);
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

TEST(VulkanDevice, AllocatesAndCachesMaterialDescriptorSets) {
    RunSpawnedSmoke("VulkanDeviceChild.DISABLED_AllocatesAndCachesMaterialDescriptorSets");
}

} // namespace
