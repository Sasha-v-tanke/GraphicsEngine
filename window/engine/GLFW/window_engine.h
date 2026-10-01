#pragma once

#include <glfw.h>
#include <memory>
#include <string>
#include <vector>

#include <window/engine/engine.h>
#include <window/window_config.h>

namespace NWindow::NEngine::NGlfw {

class IGlfwWindowEngine: public IWindowEngine {
public:
    [[nodiscard]] virtual GLFWwindow& GetGlfwWindow() = 0;
};

[[nodiscard]] std::vector<std::string> GetRequiredVulkanInstanceExtensions();

[[nodiscard]] std::unique_ptr<IWindowEngine> CreateWindowEngine(const WindowConfig& config);

} // namespace NWindow::NEngine::NGlfw
