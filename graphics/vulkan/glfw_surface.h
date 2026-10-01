#pragma once

#include <memory>
#include <string>
#include <vector>
#include <vulkan.h>

#include <GraphicsEngine/graphics/vulkan/instance.h>
#include <GraphicsEngine/lib/common/wrapper/non_transferable.h>
#include <GraphicsEngine/window/window.h>

namespace NVulkan::NGlfw {

[[nodiscard]] std::vector<std::string> GetRequiredInstanceExtensions();

class VulkanSurface final: public NCommon::NonTransferable {
public:
    VulkanSurface(const VulkanInstance& instance, NWindow::Window& window);

    ~VulkanSurface();

    [[nodiscard]] VkSurfaceKHR GetHandle() const noexcept;

private:
    class Impl;

    std::unique_ptr<Impl> m_impl;
};

} // namespace NVulkan::NGlfw
