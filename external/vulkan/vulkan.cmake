find_package(
    Vulkan
    REQUIRED
)

find_package(
    volk
    CONFIG
    REQUIRED
)

if (TARGET Volk::volk)
    message(FATAL_ERROR
        "found GNU Radio VOLK package, but GraphicsEngine requires Vulkan Volk; install Homebrew package 'vulkan-volk' instead of 'volk'"
    )
elseif (TARGET volk::volk)
    set(GraphicsEngineExternalVolkTarget volk::volk)
elseif (TARGET volk)
    set(GraphicsEngineExternalVolkTarget volk)
else ()
    message(FATAL_ERROR
        "volk package does not provide Vulkan Volk target volk::volk or volk"
    )
endif ()

add_library(
    GraphicsEngineExternalVulkan
    INTERFACE
)

add_library(
    GraphicsEngine::Vulkan
    ALIAS
    GraphicsEngineExternalVulkan
)

target_sources(
    GraphicsEngineExternalVulkan
    INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/vulkan.h>"
)

target_include_directories(
    GraphicsEngineExternalVulkan
    INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>
)

target_link_libraries(
    GraphicsEngineExternalVulkan
    INTERFACE
    Vulkan::Vulkan
    ${GraphicsEngineExternalVolkTarget}
)

set_target_properties(
    GraphicsEngineExternalVulkan
    PROPERTIES
    EXPORT_NAME "_VulkanDependencies"
    GRAPHICS_ENGINE_EXPORTABLE TRUE
)

set_property(
    TARGET GraphicsEngineExternalVulkan
    APPEND
    PROPERTY GRAPHICS_ENGINE_PACKAGE_DEPENDENCIES
    "Vulkan"
    "volk|CONFIG"
)
