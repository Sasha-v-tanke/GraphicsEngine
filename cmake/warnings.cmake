option(
    GRAPHICS_ENGINE_WARNINGS_AS_ERRORS
    "Treat GraphicsEngine compiler warnings as errors"
    OFF
)

add_library(
    GraphicsEngineWarnings
    INTERFACE
)

add_library(
    GraphicsEngine::Warnings
    ALIAS
    GraphicsEngineWarnings
)

if (
    CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
    OR
    CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang"
)
    target_compile_options(
        GraphicsEngineWarnings
        INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wconversion
        -Wsign-conversion
        -Wformat=2
        -Wundef
        -Wnon-virtual-dtor
        -Woverloaded-virtual
        -Wimplicit-fallthrough
    )

    if (GRAPHICS_ENGINE_WARNINGS_AS_ERRORS)
        target_compile_options(
            GraphicsEngineWarnings
            INTERFACE
            -Werror
        )
    endif ()
endif ()