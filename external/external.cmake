include_external(glm)
include_external(tiny_obj)
include_external(stb_image)

if (GRAPHICS_ENGINE_BUILD_TESTS)
    include_external(gtest)
endif ()

if (GRAPHICS_ENGINE_BUILD_BENCHMARKS)
    include_external(benchmark)
endif ()

if (GRAPHICS_ENGINE_BUILD_GLFW)
    include_external(glfw)
endif ()

if (GRAPHICS_ENGINE_BUILD_VULKAN)
    include_external(vulkan)
endif ()
