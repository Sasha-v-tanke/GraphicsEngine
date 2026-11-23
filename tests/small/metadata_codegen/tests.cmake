add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/fixture" "${PROJECT_BINARY_DIR}/metadata-fixture")
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
add_test(NAME GraphicsEngineMetadataGenerator
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/test_generator.py"
        --generator "${PROJECT_SOURCE_DIR}/tools/metadata_codegen/generate.py"
        --compiler "${CMAKE_CXX_COMPILER}"
        --include "${PROJECT_BINARY_DIR}/include"
)
set_tests_properties(GraphicsEngineMetadataGenerator PROPERTIES LABELS "small;metadata" TIMEOUT 120)
add_test(NAME GraphicsEngineMetadataIncremental
    COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIRECTORY=${PROJECT_BINARY_DIR}"
        "-DTEST_CONFIG=$<CONFIG>" -P "${CMAKE_CURRENT_LIST_DIR}/incremental_test.cmake")
set_tests_properties(GraphicsEngineMetadataIncremental PROPERTIES LABELS "small;metadata" TIMEOUT 120 RUN_SERIAL TRUE)
