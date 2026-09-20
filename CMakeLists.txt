cmake_minimum_required(VERSION 3.31)

project(
    GraphicsEngine
    VERSION 0.1.0
    LANGUAGES CXX
)

include(GNUInstallDirs)

option(
    GRAPHICS_ENGINE_INSTALL
    "Generate GraphicsEngine install rules"
    ${PROJECT_IS_TOP_LEVEL}
)

option(
    GRAPHICS_ENGINE_BUILD_TESTS
    "Build GraphicsEngine tests"
    ${PROJECT_IS_TOP_LEVEL}
)

option(
    GRAPHICS_ENGINE_BUILD_BENCHMARKS
    "Build GraphicsEngine benchmarks"
    ${PROJECT_IS_TOP_LEVEL}
)

option(
    GRAPHICS_ENGINE_BUILD_SAMPLES
    "Build GraphicsEngine samples"
    ${PROJECT_IS_TOP_LEVEL}
)

option(
    GRAPHICS_ENGINE_BUILD_GLFW
    "Build GraphicsEngine GLFW support"
    ON
)

option(
    GRAPHICS_ENGINE_BUILD_VULKAN
    "Build GraphicsEngine Vulkan backend"
    ON
)

set(CMAKE_CXX_EXTENSIONS OFF)

if (PROJECT_IS_TOP_LEVEL)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif ()

include(cmake/utils.cmake)
include(cmake/sanitizers.cmake)
include(cmake/warnings.cmake)

# Modules
include(external/external.cmake)
include(lib/lib.cmake)
include(window/module.cmake)
include(engine/module.cmake)
include(application/module.cmake)

PACKAGE_ROOT(Application)

if (GRAPHICS_ENGINE_INSTALL)
    include(cmake/package.cmake)
endif ()

if (GRAPHICS_ENGINE_BUILD_TESTS)
    enable_testing()
    include(tests/tests.cmake)
endif ()

if (GRAPHICS_ENGINE_BUILD_BENCHMARKS)
    include(benchmarks/benchmarks.cmake)
endif ()

if (GRAPHICS_ENGINE_BUILD_SAMPLES)
    include(samples/samples.cmake)
endif ()
