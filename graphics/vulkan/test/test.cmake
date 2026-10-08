TEST_MODULE(VulkanBackend)

SOURCES(
    device_test.cpp
    instance_test.cpp
    physical_device_test.cpp
    swapchain_test.cpp
)

if (GRAPHICS_ENGINE_BUILD_GLFW)
    SOURCES(
        device_glfw_test.cpp
        glfw_surface_test.cpp
        swapchain_glfw_test.cpp
    )
endif ()

TEST_LABELS(
    heavy
    cpp
    vulkan
)
