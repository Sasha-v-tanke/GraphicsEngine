TEST_SUITE(Engine)

TEST(Engine)

SOURCES(
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
