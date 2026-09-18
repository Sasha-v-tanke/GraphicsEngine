cmake_minimum_required(VERSION 3.31)

project(
    GraphicsEnginePackageConsumer
    LANGUAGES CXX
)

if (GRAPHICS_ENGINE_TEST_COMPONENTS)
    find_package(
        GraphicsEngine REQUIRED CONFIG
        COMPONENTS
        ${GRAPHICS_ENGINE_TEST_COMPONENTS}
    )
else ()
    find_package(
        GraphicsEngine REQUIRED CONFIG
    )
endif ()

add_executable(
    GraphicsEnginePackageConsumer
    main.cpp
)

target_link_libraries(
    GraphicsEnginePackageConsumer
    PRIVATE
    GraphicsEngine::GraphicsEngine
)

if (
    TARGET GraphicsEngine::Window
    OR TARGET GraphicsEngine::Common
    OR TARGET GraphicsEngine::Vulkan
    OR TARGET GraphicsEngine::GLFW
)
    message(FATAL_ERROR
        "GraphicsEngine package exposes module/capability targets as public API"
    )
endif ()

enable_testing()

add_test(
    NAME GraphicsEnginePackageConsumerRun
    COMMAND GraphicsEnginePackageConsumer
)
