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
    "${CMAKE_CURRENT_LIST_DIR}/vulkan.h"
)

target_include_directories(
    GraphicsEngineExternalVulkan
    INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}"
)

target_link_libraries(
    GraphicsEngineExternalVulkan
    INTERFACE
    Vulkan::Vulkan
    ${GraphicsEngineExternalVolkTarget}
)
