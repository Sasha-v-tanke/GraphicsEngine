MODULE(Renderer)

API(
    renderer.h
    render_components.h
    render_world.h
)

SOURCES(
    extraction.cpp
    renderer.cpp
)

PUBLIC_DEPENDS(
    Graphics
    Math
    Resources
)

PRIVATE_DEPENDS(
    Ecs
    Engine
)

RECURSE(
    test
)
