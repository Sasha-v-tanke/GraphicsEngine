#include "glfw_surface.h"

#include <glfw.h>
#include <string_view>
#include <vector>

#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>
#include <window/engine/GLFW/window_engine.h>
#include <window/internal/window_engine_access.h>

namespace NVulkan::NGlfw {

namespace {

std::string_view GetVkResultName(VkResult result) {
    switch (result) {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
        return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_ERROR_SURFACE_LOST_KHR:
        return "VK_ERROR_SURFACE_LOST_KHR";
    default:
        return "VK_RESULT_UNKNOWN";
    }
}

NWindow::NEngine::NGlfw::IGlfwWindowEngine& GetGlfwEngine(NWindow::Window& window) {
    NWindow::NEngine::IWindowEngine& engine = NWindow::NInternal::GetWindowEngine(window);
    auto* glfwEngine = dynamic_cast<NWindow::NEngine::NGlfw::IGlfwWindowEngine*>(&engine);
    if (glfwEngine == nullptr) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Vulkan GLFW surface requires a GLFW window engine");
    }

    return *glfwEngine;
}

} // namespace

std::vector<std::string> GetRequiredInstanceExtensions() {
    return NWindow::NEngine::NGlfw::GetRequiredVulkanInstanceExtensions();
}

class VulkanSurface::Impl final: public NCommon::NonTransferable {
public:
    Impl(const VulkanInstance& instance, NWindow::Window& window)
        : m_instance(instance.GetHandle())
        , m_surface(CreateSurface(m_instance, window)) {
    }

    ~Impl() {
        if (m_surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
        }
    }

    [[nodiscard]] VkSurfaceKHR GetHandle() const noexcept {
        return m_surface;
    }

private:
    static VkSurfaceKHR CreateSurface(VkInstance instance, NWindow::Window& window) {
        NWindow::NEngine::NGlfw::IGlfwWindowEngine& glfwEngine = GetGlfwEngine(window);

        VkSurfaceKHR surface = VK_NULL_HANDLE;
        const VkResult result = glfwCreateWindowSurface(instance, &glfwEngine.GetGlfwWindow(), nullptr, &surface);
        if (result != VK_SUCCESS) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                                  "Failed to create GLFW Vulkan surface: {}",
                                  GetVkResultName(result));
        }

        return surface;
    }

private:
    VkInstance m_instance = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
};

VulkanSurface::VulkanSurface(const VulkanInstance& instance, NWindow::Window& window)
    : m_impl(std::make_unique<Impl>(instance, window)) {
}

VulkanSurface::~VulkanSurface() = default;

VkSurfaceKHR VulkanSurface::GetHandle() const noexcept {
    return m_impl->GetHandle();
}

} // namespace NVulkan::NGlfw
