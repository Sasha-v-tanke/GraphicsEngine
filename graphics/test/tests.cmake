TEST_SUITE(Graphics)

TEST(Graphics)

SOURCES(
    submission_uses.cpp
    graphics_test.cpp
)

PRIVATE_DEPENDS(
    Graphics
    Resources
)

TEST_LABELS(
    small
    cpp
    graphics
)

RECURSE(retirement_fault)
