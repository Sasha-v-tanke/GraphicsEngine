MODULE(Scene)

API(
    static_bvh.h
    spatial_bounds.h
    scene.h
    scene_handle.h
    scene_manager.h
)

SOURCES(
    static_bvh.cpp
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
