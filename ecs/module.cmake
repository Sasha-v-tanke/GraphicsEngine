MODULE(Ecs)

API(
    entity.h
    system_access.h
    world.h
)

SOURCES(
    world.cpp
)

PUBLIC_DEPENDS(
    Common
)

RECURSE(
    test
)
