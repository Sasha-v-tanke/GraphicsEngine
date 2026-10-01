TEST_SUITE(Renderer)

SOURCES(
    render_world_test.cpp
)

PRIVATE_DEPENDS(
    Renderer
    Ecs
    Engine
    Math
    Resources
)

TEST_LABELS(
    small
    cpp
    renderer
)
