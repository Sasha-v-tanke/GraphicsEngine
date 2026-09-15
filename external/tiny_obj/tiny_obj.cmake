add_library(
    GraphicsEngineExternalTinyObj
    STATIC
)

add_library(
    GraphicsEngine::TinyObj
    ALIAS
    GraphicsEngineExternalTinyObj
)

target_sources(
    GraphicsEngineExternalTinyObj
    PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/tiny_obj_loader.h"
    "${CMAKE_CURRENT_LIST_DIR}/tiny_obj_loader.cpp"
)

target_include_directories(
    GraphicsEngineExternalTinyObj
    PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}"
)
