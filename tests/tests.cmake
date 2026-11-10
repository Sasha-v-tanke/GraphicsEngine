if (APPLE AND GRAPHICS_ENGINE_BUILD_GLFW)
    enable_language(OBJCXX)
    add_executable(
        GraphicsEngineCheckWindowFocus
        "${CMAKE_CURRENT_LIST_DIR}/common/check_window_focus.mm"
    )
    set_target_properties(
        GraphicsEngineCheckWindowFocus
        PROPERTIES
        OBJCXX_STANDARD 23
        OBJCXX_STANDARD_REQUIRED ON
    )
    _GRAPHICS_ENGINE_APPLY_PROJECT_OPTIONS(GraphicsEngineCheckWindowFocus)
    target_link_libraries(GraphicsEngineCheckWindowFocus PRIVATE "-framework AppKit")
    add_test(
        NAME GraphicsEngineWindowFrontmostProcess
        COMMAND GraphicsEngineCheckWindowFocus $<TARGET_FILE:GraphicsEngineWindowFocusTests>
    )
    set_tests_properties(
        GraphicsEngineWindowFrontmostProcess
        PROPERTIES
        LABELS "medium;window"
        SKIP_RETURN_CODE 77
        TIMEOUT 60
    )
endif ()

include(
    "${CMAKE_CURRENT_LIST_DIR}/small/package/package.cmake"
)

RECURSE(
    medium/cpp
)
