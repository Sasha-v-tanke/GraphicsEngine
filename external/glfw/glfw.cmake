find_package(
    glfw3
    CONFIG
    REQUIRED
)

add_library(
    GraphicsEngineExternalGLFW
    INTERFACE
)

add_library(
    GraphicsEngine::GLFW
    ALIAS
    GraphicsEngineExternalGLFW
)

target_sources(
    GraphicsEngineExternalGLFW
    INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/glfw.h"
)

target_include_directories(
    GraphicsEngineExternalGLFW
    INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}"
)

target_link_libraries(
    GraphicsEngineExternalGLFW
    INTERFACE
    glfw
)
