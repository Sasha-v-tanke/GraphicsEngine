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
    small
    cpp
    libs
)