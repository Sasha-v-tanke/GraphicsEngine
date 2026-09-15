cmake_minimum_required(VERSION 4.3)

project(
    GraphicsEngine
    LANGUAGES CXX
)

option(
    GRAPHICS_ENGINE_BUILD_TESTS
    "Build GraphicsEngine tests"
    ON
)

option(
    GRAPHICS_ENGINE_BUILD_VULKAN
    "Build GraphicsEngine Vulkan backend"
    ON
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/utils.cmake)

if (GRAPHICS_ENGINE_BUILD_TESTS)
    enable_testing()
endif ()

# Modules
include(external/external.cmake)

if (GRAPHICS_ENGINE_BUILD_TESTS)
    include(tests/tests.cmake)
endif ()

include(samples/samples.cmake)
