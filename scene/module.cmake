MODULE(Scene)

API(
    spatial_bounds.h
    scene.h
    scene_handle.h
    scene_manager.h
)

SOURCES(
    spatial_bounds.cpp
    scene_manager.cpp
)

PUBLIC_DEPENDS(
    Ecs
    Common
    Math
)

RECURSE(
    test
)
