RECURSE(
    sample_1_window
    sample_2_application
    sample_3_runtime_mvp
)

if (GRAPHICS_ENGINE_BUILD_VULKAN AND GRAPHICS_ENGINE_BUILD_GLFW)
    RECURSE(sample_4_clear_frame sample_5_triangle)
endif ()
