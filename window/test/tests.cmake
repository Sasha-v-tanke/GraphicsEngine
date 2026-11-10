TEST_SUITE(Window)

TEST(Window)

SOURCES(
    window_test.cpp
)

PRIVATE_DEPENDS(
    Window
)

TEST_LABELS(
    small
    cpp
    window
)

if (GRAPHICS_ENGINE_BUILD_GLFW)
    TEST_SUITE(WindowFocus)

    SOURCES(
        window_focus.cpp
    )

    PRIVATE_DEPENDS(
        Window
        GLFW
    )

    TEST_LABELS(
        medium
        cpp
        window
    )
endif ()
