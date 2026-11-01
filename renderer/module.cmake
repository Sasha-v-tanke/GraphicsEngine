MODULE(Renderer)

API(
    render_graph.h
    renderer.h
    render_components.h
    render_world.h
)

SOURCES(
    execution_validation.h
    execution_validation.cpp
    render_graph.cpp
    extraction.cpp
    renderer.cpp
)

PUBLIC_DEPENDS(
    Graphics
    Math
    Resources
    Thread
)

PRIVATE_DEPENDS(
    Ecs
    Engine
)

RECURSE(
    test
)

if (GRAPHICS_ENGINE_BUILD_VULKAN AND GRAPHICS_ENGINE_BUILD_GLFW)
    RECURSE(presentation)
endif ()
