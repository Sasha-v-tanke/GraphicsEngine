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
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>
)

set_target_properties(
    GraphicsEngineExternalTinyObj
    PROPERTIES
    EXPORT_NAME "_TinyObj"
    GRAPHICS_ENGINE_EXPORTABLE TRUE
)
