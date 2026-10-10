MODULE(VulkanBackend)

API(
    device.h
    frame_context.h
    instance.h
    physical_device.h
    resource_conversion.h
    resource_state.h
    submission_manager.h
    swapchain.h
)

SOURCES(
    device.cpp
    frame_context.cpp
    instance.cpp
    physical_device.cpp
    resource_conversion.cpp
    resource_state.cpp
    submission_manager.cpp
    swapchain.cpp
)

PUBLIC_DEPENDS(
    Common
    Graphics
    Vulkan
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
