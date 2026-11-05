MODULE(Ecs)

API(
    entity.h
    hierarchy.h
    system_access.h
    world.h
)

SOURCES(
    internal/scoped_batch.h
    internal/scoped_batch.cpp
    internal/system_run.h
    internal/system_run.cpp
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
