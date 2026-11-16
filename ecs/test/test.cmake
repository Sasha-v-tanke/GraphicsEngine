TEST_SUITE(Ecs)

SOURCES(
    entity_ref.cpp
    structural_commit.cpp
    transform_resolver.cpp
    adaptive_traversal.cpp
    change_tracking.cpp
    access_coordinator.cpp
    world_test.cpp
    hierarchy.cpp
    scheduling_test.cpp
    scoped_batch.cpp
)

PRIVATE_DEPENDS(
    Ecs
)

TEST_LABELS(
    small
    cpp
    ecs
)

RECURSE(run_fault)
