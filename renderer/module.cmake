MODULE(Renderer)

API(
    render_components.h
    render_world.h
)

SOURCES(
    extraction.cpp
)

PUBLIC_DEPENDS(
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
