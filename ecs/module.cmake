MODULE(Ecs)

API(
    entity.h
    system_access.h
    world.h
)

SOURCES(
    world.cpp
)

RECURSE(
    test
)
