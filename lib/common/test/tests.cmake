TEST_SUITE(Common)

SOURCES(change_tracker.cpp)

PRIVATE_DEPENDS(
    Common
)

RECURSE(
    error
    wrapper
)

TEST_LABELS(
    small
    cpp
    common
)
