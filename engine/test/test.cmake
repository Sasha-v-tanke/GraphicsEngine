TEST_SUITE(Engine)

TEST(Engine)

SOURCES(
    async_update.cpp
    engine_test.cpp
    engine_state_manager.cpp
    user_jobs.cpp
    user_tasks.cpp
    job_await.cpp
    frame_scheduler_test.cpp
)

PRIVATE_DEPENDS(
    Engine
    Ecs
)

TEST_LABELS(
    small
    cpp
    engine
)

RECURSE(admission_fault)
