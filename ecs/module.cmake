MODULE(Ecs)

API(
    entity.h
    system_access.h
    world.h
)

SOURCES(
    world.cpp
    internal/access_coordinator.h
    internal/access_coordinator.cpp
)

PUBLIC_DEPENDS(
    Common
    Thread
)

RECURSE(
    test
)
