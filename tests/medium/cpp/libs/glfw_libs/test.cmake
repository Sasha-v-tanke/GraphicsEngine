TESTS(GLFWLibs)

TEST(GLFWLibs)

SOURCES(
    glfw_libs.cpp
)

PRIVATE_DEPENDS(
    GLFW
)

TEST_LABELS(
    medium
    cpp
    libs
)
