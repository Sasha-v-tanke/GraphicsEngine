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
    "${CMAKE_CURRENT_LIST_DIR}/glm.h"
)

target_include_directories(
    GraphicsEngineExternalGLM
    INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}"
)
