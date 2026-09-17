add_library(
    GraphicsEngineExternalStbImage
    STATIC
)

add_library(
    GraphicsEngine::StbImage
    ALIAS
    GraphicsEngineExternalStbImage
)

target_sources(
    GraphicsEngineExternalStbImage
    PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/stb_image.h"
    "${CMAKE_CURRENT_LIST_DIR}/stb_image.cpp"
)

target_include_directories(
    GraphicsEngineExternalStbImage
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>
)

set_target_properties(
    GraphicsEngineExternalStbImage
    PROPERTIES
    EXPORT_NAME "_StbImage"
    GRAPHICS_ENGINE_EXPORTABLE TRUE
)
