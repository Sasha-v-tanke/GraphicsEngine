MODULE(VulkanBackend)

API(
    descriptor_manager.h
    device.h
    instance.h
    physical_device.h
    resource_conversion.h
    swapchain.h
)

SOURCES(
    descriptor_manager.cpp
    device.cpp
    instance.cpp
    physical_device.cpp
    resource_conversion.cpp
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
