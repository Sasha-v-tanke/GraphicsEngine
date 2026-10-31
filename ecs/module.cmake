MODULE(Ecs)

API(
    entity.h
    hierarchy.h
    system_access.h
    world.h
)

SOURCES(
    world.cpp
    hierarchy.cpp
    internal/topology.h
    internal/access_coordinator.h
    internal/access_coordinator.cpp
)

PUBLIC_DEPENDS(
    Common
    Thread
    Math
)

RECURSE(
    test
)
