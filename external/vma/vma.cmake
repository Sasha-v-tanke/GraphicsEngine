add_library(
    GraphicsEngineExternalVma
    STATIC
)

add_library(
    GraphicsEngine::VulkanMemoryAllocator
    ALIAS
    GraphicsEngineExternalVma
)

target_sources(
    GraphicsEngineExternalVma
    PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/vk_mem_alloc.h"
    "${CMAKE_CURRENT_LIST_DIR}/vma.cpp"
)

target_include_directories(
    GraphicsEngineExternalVma
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>
)

target_link_libraries(
    GraphicsEngineExternalVma
    PUBLIC
    GraphicsEngine::Vulkan
)

set_target_properties(
    GraphicsEngineExternalVma
    PROPERTIES
    EXPORT_NAME "_VulkanMemoryAllocator"
    GRAPHICS_ENGINE_EXPORTABLE TRUE
)
