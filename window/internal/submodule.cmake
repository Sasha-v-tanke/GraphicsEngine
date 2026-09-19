SUBMODULE()

SOURCES(
    factory.cpp
)

if (GRAPHICS_ENGINE_BUILD_GLFW)
    target_compile_definitions(
        ${GRAPHICS_ENGINE_CURRENT_TARGET}
        PRIVATE
        GRAPHICS_ENGINE_WINDOW_HAS_GLFW
    )

    PRIVATE_DEPENDS(
        GLFW
    )

    RECURSE(
        glfw
    )
endif ()
