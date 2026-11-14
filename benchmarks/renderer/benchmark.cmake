BENCHMARK(Renderer)

SOURCES(
    extraction.cpp
    preparation.cpp
)

PRIVATE_DEPENDS(
    Renderer
    Ecs
    Engine
    Resources
    Graphics
    Thread
)
