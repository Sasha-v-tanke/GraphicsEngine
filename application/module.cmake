MODULE(Application)

API(
    application.h
    application_config.h
    frame_diagnostics.h
)

SOURCES(
    application.cpp
)

PUBLIC_DEPENDS(
    Window
    Resources
    Graphics
    Common
    Thread
    Math
    Ecs
    Scene
    Renderer
)

PRIVATE_DEPENDS(
    Engine
)

if (GRAPHICS_ENGINE_BUILD_VULKAN AND GRAPHICS_ENGINE_BUILD_GLFW)
    PRIVATE_DEPENDS(VulkanBackend)
    target_compile_definitions(GraphicsEngineApplication PRIVATE GRAPHICS_ENGINE_HAS_PRESENTATION=1)
endif ()

RECURSE(
    runtime
    test
)
