TESTS(CommonLibs)

TEST(CommonLibs)

SOURCES(
    common_libs.cpp
)

PRIVATE_DEPENDS(
    GLM
    StbImage
    TinyObj
)

TEST_LABELS(
    medium
    cpp
    libs
)
