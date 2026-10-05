MODULE(VulkanBackend)

API(
    device.h
    instance.h
    physical_device.h
    resource_conversion.h
)

SOURCES(
    device.cpp
    instance.cpp
    physical_device.cpp
    resource_conversion.cpp
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
