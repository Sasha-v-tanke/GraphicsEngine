TEST_SUITE(Engine)

TEST(Engine)

SOURCES(
    engine_test.cpp
    frame_scheduler_test.cpp
)

PRIVATE_DEPENDS(
    Engine
)

TEST_LABELS(
    small
    cpp
    engine
)
