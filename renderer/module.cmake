MODULE(Renderer)

API(
    render_graph.h
    renderer.h
    render_components.h
    render_world.h
)

SOURCES(
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
