TEST_SUITE(Scene)

SOURCES(
    entity_ref.cpp
    scene_test.cpp
)

PRIVATE_DEPENDS(
    Scene
    Renderer
    Resources
    Graphics
    Engine
)

TEST_LABELS(
    small
    cpp
    scene
)
