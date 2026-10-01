MODULE(VulkanBackend)

API(
    instance.h
)

SOURCES(
    instance.cpp
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
