TEST_MODULE(VulkanBackend)

SOURCES(
    device_test.cpp
    frame_context_test.cpp
    instance_test.cpp
    physical_device_test.cpp
    swapchain_test.cpp
)

if (GRAPHICS_ENGINE_BUILD_GLFW)
    SOURCES(
        glfw_surface_test.cpp
    )
endif ()

TEST_LABELS(
    heavy
    cpp
    vulkan
)
