MODULE(Scene)

API(
    scene.h
    scene_handle.h
    scene_manager.h
)

SOURCES(
    scene_manager.cpp
)

PUBLIC_DEPENDS(
    Ecs
    Common
)

RECURSE(
    test
)
