function(_GRAPHICS_ENGINE_RUN description)
    execute_process(
        COMMAND
        ${ARGN}
        RESULT_VARIABLE
        result
        OUTPUT_VARIABLE
        output
        ERROR_VARIABLE
        error
    )

    if (NOT result EQUAL 0)
        message(FATAL_ERROR
            "${description} failed\n"
            "${output}\n"
            "${error}"
        )
    endif ()
endfunction()


set(
    testDirectory
    "${GRAPHICS_ENGINE_BINARY_DIR}/package-test"
)

set(
    installDirectory
    "${testDirectory}/install"
)

set(
    consumerBuildDirectory
    "${testDirectory}/consumer"
)

set(
    invalidComponentBuildDirectory
    "${testDirectory}/invalid-component"
)

file(
    REMOVE_RECURSE
    "${testDirectory}"
)


# =============================================================================
# Install
# =============================================================================

set(installConfigurationArguments)

if (GRAPHICS_ENGINE_TEST_CONFIG)
    list(APPEND
        installConfigurationArguments
        --config
        "${GRAPHICS_ENGINE_TEST_CONFIG}"
    )
endif ()

_GRAPHICS_ENGINE_RUN(
    "GraphicsEngine installation"
    "${CMAKE_COMMAND}"
    --install
    "${GRAPHICS_ENGINE_BINARY_DIR}"
    --prefix
    "${installDirectory}"
    ${installConfigurationArguments}
)


# =============================================================================
# Verify installed API
# =============================================================================

if (
    NOT EXISTS
    "${installDirectory}/include/GraphicsEngine/window/window.h"
)
    message(FATAL_ERROR
        "Public Window header was not installed"
    )
endif ()

if (
    EXISTS
    "${installDirectory}/include/GraphicsEngine/window/internal/factory.h"
)
    message(FATAL_ERROR
        "Internal Window header was installed"
    )
endif ()

if (
    EXISTS
    "${installDirectory}/include/GraphicsEngine/lib/common/error/error.h"
)
    message(FATAL_ERROR
        "Private Common dependency leaked into public API"
    )
endif ()


# =============================================================================
# Consumer
# =============================================================================

set(consumerConfigureArguments)

if (GRAPHICS_ENGINE_TEST_CONFIG)
    list(APPEND
        consumerConfigureArguments
        "-DCMAKE_BUILD_TYPE=${GRAPHICS_ENGINE_TEST_CONFIG}"
    )
endif ()

_GRAPHICS_ENGINE_RUN(
    "Consumer configuration"
    "${CMAKE_COMMAND}"
    -S
    "${GRAPHICS_ENGINE_SOURCE_DIR}/tests/small/package/consumer"
    -B
    "${consumerBuildDirectory}"
    "-DCMAKE_PREFIX_PATH=${installDirectory}"
    ${consumerConfigureArguments}
)

set(consumerBuildArguments)

if (GRAPHICS_ENGINE_TEST_CONFIG)
    list(APPEND
        consumerBuildArguments
        --config
        "${GRAPHICS_ENGINE_TEST_CONFIG}"
    )
endif ()

_GRAPHICS_ENGINE_RUN(
    "Consumer build"
    "${CMAKE_COMMAND}"
    --build
    "${consumerBuildDirectory}"
    ${consumerBuildArguments}
)

set(consumerTestArguments)

if (GRAPHICS_ENGINE_TEST_CONFIG)
    list(APPEND
        consumerTestArguments
        -C
        "${GRAPHICS_ENGINE_TEST_CONFIG}"
    )
endif ()

_GRAPHICS_ENGINE_RUN(
    "Consumer run"
    "${GRAPHICS_ENGINE_CTEST_COMMAND}"
    --test-dir
    "${consumerBuildDirectory}"
    --output-on-failure
    ${consumerTestArguments}
)


# =============================================================================
# Invalid component
# =============================================================================

execute_process(
    COMMAND
    "${CMAKE_COMMAND}"
    -S
    "${GRAPHICS_ENGINE_SOURCE_DIR}/tests/small/package/consumer"
    -B
    "${invalidComponentBuildDirectory}"
    "-DCMAKE_PREFIX_PATH=${installDirectory}"
    "-DGRAPHICS_ENGINE_TEST_COMPONENTS=DefinitelyMissing"
    ${consumerConfigureArguments}
    RESULT_VARIABLE
    invalidComponentResult
    OUTPUT_QUIET
    ERROR_QUIET
)

if (invalidComponentResult EQUAL 0)
    message(FATAL_ERROR
        "GraphicsEngine accepted an unavailable required component"
    )
endif ()
