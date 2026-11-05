BENCHMARK_SUITE(Main)

RECURSE(
    smoke
    task
    renderer
)

if (GRAPHICS_ENGINE_BUILD_VULKAN AND GRAPHICS_ENGINE_BUILD_GLFW)
    RECURSE(
        upload
    )
endif ()
