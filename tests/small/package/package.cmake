if (GRAPHICS_ENGINE_INSTALL)
    add_test(
        NAME GraphicsEnginePackage
        COMMAND
        ${CMAKE_COMMAND}
        "-DGRAPHICS_ENGINE_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DGRAPHICS_ENGINE_BINARY_DIR=${PROJECT_BINARY_DIR}"
        "-DGRAPHICS_ENGINE_TEST_CONFIG=$<CONFIG>"
        "-DGRAPHICS_ENGINE_CTEST_COMMAND=${CMAKE_CTEST_COMMAND}"
        -P
        "${CMAKE_CURRENT_LIST_DIR}/package_test.cmake"
    )

    set_tests_properties(
        GraphicsEnginePackage
        PROPERTIES
        LABELS "small;package"
    )
endif ()
