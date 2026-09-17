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
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/glfw.h>"
)

target_include_directories(
    GraphicsEngineExternalGLFW
    INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>
)

target_link_libraries(
    GraphicsEngineExternalGLFW
    INTERFACE
    glfw
)

set_target_properties(
    GraphicsEngineExternalGLFW
    PROPERTIES
    EXPORT_NAME "_GLFWDependencies"
    GRAPHICS_ENGINE_EXPORTABLE TRUE
)

PACKAGE_DEPENDS(
    GraphicsEngineExternalGLFW
    glfw3
    CONFIG
)
