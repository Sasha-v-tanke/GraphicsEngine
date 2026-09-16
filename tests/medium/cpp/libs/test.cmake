RECURSE(
    common_libs
)

if (GRAPHICS_ENGINE_BUILD_GLFW)
    RECURSE(
        glfw_libs
    )
endif ()

if (GRAPHICS_ENGINE_BUILD_VULKAN)
    RECURSE(
        vulkan_libs
    )
endif ()

