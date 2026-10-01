TEST_MODULE(VulkanBackend)

SOURCES(
    instance_test.cpp
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
