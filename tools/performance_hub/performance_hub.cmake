find_package(SQLite3 REQUIRED)

add_library(GraphicsEnginePerformanceHubStorage STATIC
    "${CMAKE_CURRENT_LIST_DIR}/storage.cpp"
)
target_compile_features(GraphicsEnginePerformanceHubStorage PUBLIC cxx_std_23)
target_include_directories(GraphicsEnginePerformanceHubStorage PUBLIC "${PROJECT_SOURCE_DIR}")
target_link_libraries(GraphicsEnginePerformanceHubStorage PRIVATE SQLite3::SQLite3)
_GRAPHICS_ENGINE_APPLY_PROJECT_OPTIONS(GraphicsEnginePerformanceHubStorage)

if (GRAPHICS_ENGINE_BUILD_TESTS)
    add_executable(GraphicsEnginePerformanceHubStorageTests
        "${CMAKE_CURRENT_LIST_DIR}/test/storage_tests.cpp"
    )
    target_compile_features(GraphicsEnginePerformanceHubStorageTests PRIVATE cxx_std_23)
    target_link_libraries(GraphicsEnginePerformanceHubStorageTests PRIVATE GraphicsEnginePerformanceHubStorage)
    target_include_directories(GraphicsEnginePerformanceHubStorageTests PRIVATE "${PROJECT_SOURCE_DIR}")
    _GRAPHICS_ENGINE_APPLY_PROJECT_OPTIONS(GraphicsEnginePerformanceHubStorageTests)
    add_test(NAME GraphicsEnginePerformanceHubStorageTests COMMAND GraphicsEnginePerformanceHubStorageTests)
endif ()
