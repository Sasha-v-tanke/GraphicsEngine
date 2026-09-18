TESTS(Common)

TEST(Common)

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
