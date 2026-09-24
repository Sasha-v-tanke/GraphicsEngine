TEST_SUITE(Application)

TEST(Application)

SOURCES(
    application_test.cpp
    runtime_mvp_test.cpp
)

PRIVATE_DEPENDS(
    Application
)

TEST_LABELS(
    small
    cpp
    application
)
