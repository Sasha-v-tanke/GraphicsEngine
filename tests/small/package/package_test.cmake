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

set(
    buildInterfaceSourceDirectory
    "${testDirectory}/build-interface-consumer"
)

set(
    buildInterfaceEngineSourceDirectory
    "${testDirectory}/engine-src"
)

set(
    buildInterfaceBuildDirectory
    "${testDirectory}/build-interface-build"
)

set(
    publicHeaderBuildDirectory
    "${testDirectory}/public-header-consumer"
)

set(
    publicHeaderSourceDirectory
    "${testDirectory}/public-header-consumer-src"
)

file(
    REMOVE_RECURSE
    "${testDirectory}"
)

file(
    MAKE_DIRECTORY
    "${testDirectory}"
)

cmake_policy(PUSH)

if (POLICY CMP0205)
    cmake_policy(SET CMP0205 NEW)
endif ()


# =============================================================================
# Build interface
# =============================================================================

file(
    CREATE_LINK
    "${GRAPHICS_ENGINE_SOURCE_DIR}"
    "${buildInterfaceEngineSourceDirectory}"
    SYMBOLIC
    COPY_ON_ERROR
)

cmake_policy(POP)

file(
    MAKE_DIRECTORY
    "${buildInterfaceSourceDirectory}"
)

file(
    WRITE
    "${buildInterfaceSourceDirectory}/CMakeLists.txt"
    [=[
cmake_minimum_required(VERSION 3.31)

project(GraphicsEngineBuildInterfaceConsumer LANGUAGES CXX)
set(CMAKE_DISABLE_FIND_PACKAGE_Python3 TRUE)

set(GRAPHICS_ENGINE_INSTALL OFF CACHE BOOL "" FORCE)
set(GRAPHICS_ENGINE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GRAPHICS_ENGINE_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(GRAPHICS_ENGINE_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(GRAPHICS_ENGINE_BUILD_GLFW OFF CACHE BOOL "" FORCE)
set(GRAPHICS_ENGINE_BUILD_VULKAN OFF CACHE BOOL "" FORCE)

add_subdirectory("${GRAPHICS_ENGINE_TEST_ENGINE_SOURCE_DIR}" engine)

add_executable(GraphicsEngineBuildInterfaceConsumer main.cpp)
target_link_libraries(GraphicsEngineBuildInterfaceConsumer PRIVATE GraphicsEngine::GraphicsEngine)
]=]
)

file(
    WRITE
    "${buildInterfaceSourceDirectory}/main.cpp"
    [=[
#include <GraphicsEngine/application/application_config.h>
#include <GraphicsEngine/window/window_config.h>

int main() {
    const NWindow::WindowConfig windowConfig{NWindow::EWindowType::GLFW};

    return windowConfig.Size.Width > 0 ? 0 : 1;
}
]=]
)

set(buildInterfaceConfigureArguments)

if (GRAPHICS_ENGINE_TEST_CONFIG)
    list(APPEND
        buildInterfaceConfigureArguments
        "-DCMAKE_BUILD_TYPE=${GRAPHICS_ENGINE_TEST_CONFIG}"
    )
endif ()

_GRAPHICS_ENGINE_RUN(
    "Build-interface consumer configuration"
    "${CMAKE_COMMAND}"
    -S
    "${buildInterfaceSourceDirectory}"
    -B
    "${buildInterfaceBuildDirectory}"
    "-DGRAPHICS_ENGINE_TEST_ENGINE_SOURCE_DIR=${buildInterfaceEngineSourceDirectory}"
    ${buildInterfaceConfigureArguments}
)

set(buildInterfaceBuildArguments)

if (GRAPHICS_ENGINE_TEST_CONFIG)
    list(APPEND
        buildInterfaceBuildArguments
        --config
        "${GRAPHICS_ENGINE_TEST_CONFIG}"
    )
endif ()

_GRAPHICS_ENGINE_RUN(
    "Build-interface consumer build"
    "${CMAKE_COMMAND}"
    --build
    "${buildInterfaceBuildDirectory}"
    ${buildInterfaceBuildArguments}
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
# Verify installed API surface
# =============================================================================

set(publicHeaderManifest
    "${installDirectory}/lib/cmake/GraphicsEngine/GraphicsEnginePublicHeaders.cmake")

_GRAPHICS_ENGINE_RUN(
    "Installed public header lint"
    "${CMAKE_COMMAND}"
    "-DGRAPHICS_ENGINE_PUBLIC_HEADER_MANIFEST=${publicHeaderManifest}"
    "-DGRAPHICS_ENGINE_INCLUDE_DIRECTORY=${installDirectory}/include"
    -P
    "${GRAPHICS_ENGINE_SOURCE_DIR}/cmake/public_api_lint.cmake"
)

_GRAPHICS_ENGINE_RUN(
    "Public API lint negative fixtures"
    "${CMAKE_COMMAND}"
    "-DGRAPHICS_ENGINE_SOURCE_DIR=${GRAPHICS_ENGINE_SOURCE_DIR}"
    "-DGRAPHICS_ENGINE_BINARY_DIR=${GRAPHICS_ENGINE_BINARY_DIR}"
    -P
    "${GRAPHICS_ENGINE_SOURCE_DIR}/tests/small/package/public_api_lint_test.cmake"
)

include("${publicHeaderManifest}")


# =============================================================================
# Isolated installed-header compilation
# =============================================================================

file(MAKE_DIRECTORY "${publicHeaderSourceDirectory}")
file(WRITE "${publicHeaderSourceDirectory}/CMakeLists.txt"
"cmake_minimum_required(VERSION 3.31)\n"
 "project(GraphicsEnginePublicHeaders LANGUAGES CXX)\n"
 "find_package(GraphicsEngine ${PROJECT_VERSION} CONFIG REQUIRED)\n"
)

set(publicHeaderIndex 0)
foreach (header IN LISTS GRAPHICS_ENGINE_PUBLIC_HEADERS)
    math(EXPR publicHeaderIndex "${publicHeaderIndex} + 1")
    set(source "${publicHeaderSourceDirectory}/header_${publicHeaderIndex}.cpp")
    file(WRITE "${source}" "#include <${header}>\n")
    file(APPEND "${publicHeaderSourceDirectory}/CMakeLists.txt"
        "add_library(GraphicsEnginePublicHeader${publicHeaderIndex} OBJECT header_${publicHeaderIndex}.cpp)\n"
        "target_link_libraries(GraphicsEnginePublicHeader${publicHeaderIndex} PRIVATE GraphicsEngine::GraphicsEngine)\n")
endforeach ()

_GRAPHICS_ENGINE_RUN(
    "Installed public header consumer configuration"
    "${CMAKE_COMMAND}"
    -S
    "${publicHeaderSourceDirectory}"
    -B
    "${publicHeaderBuildDirectory}"
    "-DCMAKE_PREFIX_PATH=${installDirectory}"
    ${consumerConfigureArguments}
)

_GRAPHICS_ENGINE_RUN(
    "Installed public header consumer build"
    "${CMAKE_COMMAND}"
    --build
    "${publicHeaderBuildDirectory}"
    ${consumerBuildArguments}
)

set(
    internalHeaders
    "GraphicsEngine/engine/controller/frame_scheduler.h"
    "GraphicsEngine/engine/runtime/frame_runtime.h"
    "GraphicsEngine/renderer/execution_validation.h"
    "GraphicsEngine/graphics/backend/completion_access.h"
    "GraphicsEngine/graphics/backend/backend.h"
    "GraphicsEngine/graphics/backend/factory.h"
    "GraphicsEngine/graphics/backend/submission_uses.h"
    "GraphicsEngine/math/internal/glm_conversion.h"
    "GraphicsEngine/window/engine/engine.h"
    "GraphicsEngine/window/engine/event_sink.h"
    "GraphicsEngine/window/engine/factory.h"
)

foreach (internalHeader IN LISTS internalHeaders)
    if (
        EXISTS
        "${installDirectory}/include/${internalHeader}"
    )
        message(FATAL_ERROR
            "Internal header '${internalHeader}' was installed"
        )
    endif ()
endforeach ()


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

set(consumerSanitizers)

if (GRAPHICS_ENGINE_ENABLE_ASAN)
    list(APPEND
        consumerSanitizers
        address
    )
endif ()

if (GRAPHICS_ENGINE_ENABLE_UBSAN)
    list(APPEND
        consumerSanitizers
        undefined
    )
endif ()

if (consumerSanitizers)
    list(
        JOIN
        consumerSanitizers
        ","
        consumerSanitizerFlags
    )

    list(APPEND
        consumerConfigureArguments
        "-DCMAKE_CXX_FLAGS=-fsanitize=${consumerSanitizerFlags} -fno-omit-frame-pointer"
        "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=${consumerSanitizerFlags}"
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

# The installed Application interface intentionally rejects the retired OnDraw override.
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${consumerBuildDirectory}"
        --target GraphicsEngineLegacyCallback ${consumerBuildArguments}
    RESULT_VARIABLE legacyCallbackResult
    OUTPUT_VARIABLE legacyCallbackOutput
    ERROR_VARIABLE legacyCallbackError
)
if (legacyCallbackResult EQUAL 0)
    message(FATAL_ERROR "GraphicsEngine accepted the retired OnDraw override")
endif ()
if (NOT "${legacyCallbackOutput}${legacyCallbackError}" MATCHES "OnDraw.*override|override.*OnDraw")
    message(FATAL_ERROR "Legacy callback failed for an unexpected reason:\n${legacyCallbackOutput}\n${legacyCallbackError}")
endif ()
