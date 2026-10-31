TEST_SUITE(Renderer)

SOURCES(
    execution_validation.cpp
    render_graph.cpp
    render_world_test.cpp
)

PRIVATE_DEPENDS(
    Renderer
    Ecs
    Engine
    Math
    Resources
    Thread
)

TEST_LABELS(
    small
    cpp
    renderer
)
