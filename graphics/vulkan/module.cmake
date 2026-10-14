MODULE(VulkanBackend)

API(
    device.h
    frame_context.h
    instance.h
    memory.h
    physical_device.h
    resource_conversion.h
    submission_manager.h
    swapchain.h
)

SOURCES(
    device.cpp
    frame_context.cpp
    instance.cpp
    memory.cpp
    physical_device.cpp
    resource_conversion.cpp
    submission_manager.cpp
    swapchain.cpp
)

PUBLIC_DEPENDS(
    Common
    Graphics
    Vulkan
    VulkanMemoryAllocator
)

if (GRAPHICS_ENGINE_BUILD_GLFW)
    API(
        glfw_surface.h
    )

    SOURCES(
        glfw_surface.cpp
    )

    PUBLIC_DEPENDS(
        Window
    )

    PRIVATE_DEPENDS(
        GLFW
    )
endif ()

RECURSE(
    test
)
