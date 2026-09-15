option(
    GRAPHICS_ENGINE_ENABLE_ASAN
    "Enable AddressSanitizer"
    OFF
)

option(
    GRAPHICS_ENGINE_ENABLE_UBSAN
    "Enable UndefinedBehaviorSanitizer"
    OFF
)

add_library(
    GraphicsEngineSanitizers
    INTERFACE
)

add_library(
    GraphicsEngine::Sanitizers
    ALIAS
    GraphicsEngineSanitizers
)

set(sanitizerList)

if (GRAPHICS_ENGINE_ENABLE_ASAN)
    list(APPEND sanitizerList address)
endif ()

if (GRAPHICS_ENGINE_ENABLE_UBSAN)
    list(APPEND sanitizerList undefined)
endif ()

if (sanitizerList)
    if (
        NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
        AND
        NOT CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang"
        AND
        NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
    )
        message(FATAL_ERROR
            "Sanitizers are not supported for compiler '${CMAKE_CXX_COMPILER_ID}'"
        )
    endif ()

    list(JOIN sanitizerList "," sanitizerFlags)

    target_compile_options(
        GraphicsEngineSanitizers
        INTERFACE
        "-fsanitize=${sanitizerFlags}"
        -fno-omit-frame-pointer
    )

    target_link_options(
        GraphicsEngineSanitizers
        INTERFACE
        "-fsanitize=${sanitizerFlags}"
    )
endif ()