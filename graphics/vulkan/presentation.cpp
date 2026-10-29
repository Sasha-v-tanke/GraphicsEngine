#include <cstdint>
#include <exception>
#include <limits>
#include <utility>

#include <graphics/backend/backend.h>
#include <graphics/backend/factory.h>
#include <graphics/presentation.h>
#include <graphics/vulkan/clear_present_path.h>
#include <graphics/vulkan/glfw_surface.h>
#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NGraphics {
namespace {

NVulkan::VulkanInstanceConfig MakeInstanceConfig(const PresentationConfig& config) {
    return {
            .RequiredExtensions = NVulkan::NGlfw::GetRequiredInstanceExtensions(),
            .ValidationMode = config.RequireValidation ? NVulkan::EValidationMode::Required
                                                       : NVulkan::EValidationMode::EnabledIfAvailable,
            .EnableDebugUtils = true,
            .DebugMessageHandler =
                    [handler = config.ValidationErrorHandler](const NVulkan::VulkanDebugMessage& message) {
                        if (handler && (message.Severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
                            handler(message.Message);
                        }
                    },
    };
}

class PresentationBackend final: public NBackend::IGraphicsBackend {
public:
    PresentationBackend(NWindow::Window& window, const PresentationConfig& config)
        : m_instance(MakeInstanceConfig(config))
        , m_surface(m_instance, window)
        , m_physicalDevice(NVulkan::SelectVulkanPhysicalDevice(m_instance, m_surface.GetHandle()))
        , m_device(m_physicalDevice)
        , m_swapchain(m_device, m_physicalDevice, m_surface.GetHandle(), window.GetFramebufferSize())
        , m_path(m_device, m_swapchain, config.MaxActiveFrames)
        , m_capabilities(m_device.GetGraphicsCapabilities()) {
        m_capabilities.MaxFramesInFlight = static_cast<std::uint32_t>(config.MaxActiveFrames);
    }

    [[nodiscard]] const GraphicsCapabilities& GetCapabilities() const noexcept override {
        return m_capabilities;
    }

    std::uint64_t SubmitFrame(FrameSubmission) override {
        Unsupported();
    }

    NBackend::BackendFrameResult TrySubmitFrame(FrameSubmission submission) override {
        if (submission.FrameSlotIndex != submission.FrameIndex % m_capabilities.MaxFramesInFlight ||
            !submission.RequiresPresentation || !submission.ResourceUseRecords.empty()) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Invalid clear-only presentation submission");
        }
        NBackend::BackendFrameResult result;
        try {
            (void)m_path.PollCompletedFrames();
            VkClearColorValue color{};
            for (std::size_t index = 0; index < submission.ClearColor.size(); ++index) {
                color.float32[index] = submission.ClearColor[index];
            }
            const auto rendered =
                    m_path.Render(submission.FrameIndex, color, [&result](NVulkan::VulkanQueueCompletion completion) {
                        result.CompletionValue = completion.Value;
                    });
            switch (rendered.Status) {
            case NVulkan::EVulkanClearPresentStatus::PRESENTED:
                result.Status = EFrameSubmissionStatus::PRESENTED;
                break;
            case NVulkan::EVulkanClearPresentStatus::NOT_READY:
                result.Status = EFrameSubmissionStatus::NOT_READY;
                break;
            case NVulkan::EVulkanClearPresentStatus::OUT_OF_DATE:
                result.Status = EFrameSubmissionStatus::OUT_OF_DATE;
                break;
            case NVulkan::EVulkanClearPresentStatus::SUSPENDED:
                result.Status = EFrameSubmissionStatus::SUSPENDED;
                break;
            }
        } catch (...) {
            result.Status = EFrameSubmissionStatus::FAILED;
            result.Failure = std::current_exception();
        }
        return result;
    }

    void SetPresentationSize(int width, int height) override {
        m_path.SetFramebufferSize({.Width = width, .Height = height});
    }

    [[nodiscard]] bool IsCompleted(std::uint64_t value) const override {
        return m_path.IsCompleted({.Value = value});
    }

    std::uint64_t CreateBuffer(const BufferDescriptor&) override {
        Unsupported();
    }
    void DestroyBuffer(std::uint64_t, std::optional<std::uint64_t>) noexcept override {
    }
    std::uint64_t CreateGraphicsPipeline(const GraphicsPipelineDescriptor&) override {
        Unsupported();
    }
    void DestroyGraphicsPipeline(std::uint64_t, std::optional<std::uint64_t>) noexcept override {
    }
    std::uint64_t CreateImage(const ImageDescriptor&) override {
        Unsupported();
    }
    void DestroyImage(std::uint64_t, std::optional<std::uint64_t>) noexcept override {
    }
    std::uint64_t CreateImageView(const ImageViewDescriptor&) override {
        Unsupported();
    }
    void DestroyImageView(std::uint64_t, std::optional<std::uint64_t>) noexcept override {
    }
    std::uint64_t CreateSampler(const SamplerDescriptor&) override {
        Unsupported();
    }
    void DestroySampler(std::uint64_t, std::optional<std::uint64_t>) noexcept override {
    }

private:
    [[noreturn]] static void Unsupported() {
        GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED,
                              "Clear-only backend requires TrySubmitFrame; resources require graph execution");
    }

    NVulkan::VulkanInstance m_instance;
    NVulkan::NGlfw::VulkanSurface m_surface;
    NVulkan::VulkanPhysicalDeviceSelection m_physicalDevice;
    NVulkan::VulkanDevice m_device;
    NVulkan::VulkanSwapchain m_swapchain;
    NVulkan::VulkanClearPresentPath m_path;
    GraphicsCapabilities m_capabilities;
};
} // namespace

std::unique_ptr<Graphics> CreatePresentationGraphics(NWindow::Window& window, const PresentationConfig& config) {
    if (config.MaxActiveFrames == 0 || config.MaxActiveFrames > std::numeric_limits<std::uint32_t>::max()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Invalid presentation frame count");
    }
    // Graphics is nonmovable; direct new preserves guaranteed copy elision.
    // NOLINTNEXTLINE(modernize-make-unique)
    return std::unique_ptr<Graphics>{
            new Graphics{NBackend::CreateGraphicsForBackend(std::make_unique<PresentationBackend>(window, config))}};
}
} // namespace NGraphics
