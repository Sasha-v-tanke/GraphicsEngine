MODULE(VulkanBackend)

API(
    descriptor_manager.h
    device.h
    clear_present_path.h
    frame_context.h
    instance.h
    physical_device.h
    resource_conversion.h
    resource_state.h
    submission_manager.h
    swapchain.h
)

SOURCES(
    descriptor_manager.cpp
    graphics_pipeline.h
    graphics_pipeline.cpp
    device.cpp
    clear_present_path.cpp
    frame_context.cpp
    instance.cpp
    memory.h
    memory.cpp
    physical_device.cpp
    resource_conversion.cpp
    resource_state.cpp
    submission_manager.cpp
    swapchain.cpp
    upload_manager.h
    upload_manager.cpp
)

PUBLIC_DEPENDS(
    Common
    Graphics
    Vulkan
)

PRIVATE_DEPENDS(
    VulkanMemoryAllocator
)

if (GRAPHICS_ENGINE_BUILD_GLFW)
    API(
        glfw_surface.h
    )

    SOURCES(
        glfw_surface.cpp
    )

    PUBLIC_DEPENDS(
        Window
    )

    PRIVATE_DEPENDS(
        GLFW
    )
endif ()

RECURSE(
    test
)
