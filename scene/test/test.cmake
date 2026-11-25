TEST_SUITE(Scene)

SOURCES(
    static_bvh.cpp
    bvh_fixture.h
    spatial_bounds.cpp
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

RECURSE(scale)
