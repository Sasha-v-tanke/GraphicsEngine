TEST_SUITE(Ecs)

SOURCES(
    access_coordinator.cpp
    world_test.cpp
    scheduling_test.cpp
)

PRIVATE_DEPENDS(
    Ecs
)

TEST_LABELS(
    small
    cpp
    ecs
)
