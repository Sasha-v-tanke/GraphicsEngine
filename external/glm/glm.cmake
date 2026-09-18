find_package(
    glm
    CONFIG
    REQUIRED
)

add_library(
    GraphicsEngineExternalGLM
    INTERFACE
)

add_library(
    GraphicsEngine::GLM
    ALIAS
    GraphicsEngineExternalGLM
)

target_link_libraries(
    GraphicsEngineExternalGLM
    INTERFACE
    glm::glm
)

target_sources(
    GraphicsEngineExternalGLM
    INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/glm.h>"
)

target_include_directories(
    GraphicsEngineExternalGLM
    INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>
)

set_target_properties(
    GraphicsEngineExternalGLM
    PROPERTIES
    EXPORT_NAME "_GLM"
    GRAPHICS_ENGINE_EXPORTABLE TRUE
)

PACKAGE_DEPENDS(
    GraphicsEngineExternalGLM
    glm
    CONFIG
)
