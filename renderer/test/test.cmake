TEST_SUITE(Renderer)

SOURCES(
    incremental_extraction.cpp
    resource_readiness.cpp
    execution_validation.cpp
    model.cpp
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
