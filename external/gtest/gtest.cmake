find_package(
    GTest
    CONFIG
    REQUIRED
)

add_library(
    GraphicsEngineExternalTest
    INTERFACE
)

add_library(
    GraphicsEngine::Test
    ALIAS
    GraphicsEngineExternalTest
)

target_link_libraries(
    GraphicsEngineExternalTest
    INTERFACE
    GTest::gtest_main
)
