find_package(Threads REQUIRED)

add_library(GraphicsEngineExternalSystemThreads INTERFACE)
add_library(GraphicsEngine::SystemThreads ALIAS GraphicsEngineExternalSystemThreads)

target_link_libraries(GraphicsEngineExternalSystemThreads INTERFACE Threads::Threads)

set_target_properties(
    GraphicsEngineExternalSystemThreads
    PROPERTIES
    EXPORT_NAME "_SystemThreads"
    GRAPHICS_ENGINE_EXPORTABLE TRUE
)

PACKAGE_DEPENDS(GraphicsEngineExternalSystemThreads Threads)
