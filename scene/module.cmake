MODULE(Scene)

API(
    dynamic_aabb_tree.h
    static_bvh.h
    spatial_bounds.h
    scene.h
    scene_handle.h
    scene_manager.h
)

SOURCES(
    dynamic_aabb_tree.cpp
    spatial_query_detail.h
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
