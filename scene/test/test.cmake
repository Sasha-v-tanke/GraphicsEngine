TEST_SUITE(Scene)

SOURCES(
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
