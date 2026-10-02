TEST_SUITE(Graphics)

TEST(Graphics)

SOURCES(
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
