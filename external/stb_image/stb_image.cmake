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
    "${CMAKE_CURRENT_LIST_DIR}"
)
