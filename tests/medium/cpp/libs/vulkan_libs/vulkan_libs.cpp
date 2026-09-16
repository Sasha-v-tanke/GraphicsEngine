#include "vulkan.h"

#include <iostream>

int main() {
    VkInstance instance{VK_NULL_HANDLE};

    std::cout << "GraphicsEngine Vulkan libraries smoke test\n"
              << "vulkan instance: " << instance << '\n';

    return 0;
}
