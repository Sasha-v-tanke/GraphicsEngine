MODULE(VulkanBackend)

API(
    instance.h
    physical_device.h
)

SOURCES(
    instance.cpp
    physical_device.cpp
)

PUBLIC_DEPENDS(
    Common
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
