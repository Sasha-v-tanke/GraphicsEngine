include_guard(GLOBAL)

macro(TEST_SUITE name)
    if (NOT GRAPHICS_ENGINE_BUILD_TESTS)
        message(FATAL_ERROR
            "TESTS: tests are disabled"
        )
    endif ()

    set(
        testTarget
        "GraphicsEngine${name}Tests"
    )

    if (TARGET "${testTarget}")
        message(FATAL_ERROR
            "TESTS: target '${testTarget}' already exists"
        )
    endif ()

    add_executable(
        ${testTarget}
    )

    _GRAPHICS_ENGINE_APPLY_PROJECT_OPTIONS(
        ${testTarget}
    )

    add_executable(
        GraphicsEngine::${name}Tests
        ALIAS
        ${testTarget}
    )

    target_compile_features(
        ${testTarget}
        PRIVATE
        cxx_std_23
    )

    target_link_libraries(
        ${testTarget}
        PRIVATE
        GraphicsEngine::Test
    )

    target_include_directories(
        ${testTarget}
        PRIVATE
        "${PROJECT_SOURCE_DIR}"
    )

    add_test(
        NAME ${testTarget}
        COMMAND ${testTarget}
    )

    set(
        GRAPHICS_ENGINE_CURRENT_CONTEXT
        "TESTS"
    )

    set(
        GRAPHICS_ENGINE_CURRENT_MODULE
        "${name}"
    )

    set(
        GRAPHICS_ENGINE_CURRENT_TARGET
        "${testTarget}"
    )

    set(
        GRAPHICS_ENGINE_CURRENT_TEST
        ""
    )

    _GRAPHICS_ENGINE_ADD_LOCAL_HEADERS()
endmacro()


function(TEST)
    if (NOT GRAPHICS_ENGINE_BUILD_TESTS)
        message(FATAL_ERROR
            "TEST: tests are disabled"
        )
    endif ()

    _GRAPHICS_ENGINE_REQUIRE_CONTEXT(
        "TEST"
    )

    if (
        NOT GRAPHICS_ENGINE_CURRENT_CONTEXT STREQUAL "TESTS"
        AND
        NOT GRAPHICS_ENGINE_CURRENT_CONTEXT STREQUAL "TEST"
    )
        message(FATAL_ERROR
            "TEST: current context is not a test suite"
        )
    endif ()

    if (ARGC GREATER 1)
        message(FATAL_ERROR
            "TEST: expected zero or one argument"
        )
    endif ()

    if (ARGC EQUAL 1)
        set(
            GRAPHICS_ENGINE_CURRENT_TEST
            "${ARGV0}"
        )
    elseif ("${GRAPHICS_ENGINE_CURRENT_TEST}" STREQUAL "")
        message(FATAL_ERROR
            "TEST: unnamed test has no active test context"
        )
    endif ()

    set(
        GRAPHICS_ENGINE_CURRENT_CONTEXT
        "TEST"
    )

    _GRAPHICS_ENGINE_ADD_LOCAL_HEADERS()

    set(
        GRAPHICS_ENGINE_CURRENT_CONTEXT
        "${GRAPHICS_ENGINE_CURRENT_CONTEXT}"
        PARENT_SCOPE
    )

    set(
        GRAPHICS_ENGINE_CURRENT_TEST
        "${GRAPHICS_ENGINE_CURRENT_TEST}"
        PARENT_SCOPE
    )
endfunction()


macro(TEST_LABELS)
    _GRAPHICS_ENGINE_REQUIRE_CONTEXT(
        "TEST_LABELS"
    )

    if (
        NOT GRAPHICS_ENGINE_CURRENT_CONTEXT STREQUAL "TEST"
        AND
        NOT GRAPHICS_ENGINE_CURRENT_CONTEXT STREQUAL "TESTS"
    )
        message(FATAL_ERROR
            "TEST_LABELS: current context is not a test"
        )
    endif ()

    file(
        RELATIVE_PATH
        currentDirectory
        "${PROJECT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}"
    )

    set(testLabels
        ${ARGN}
        "dir:${currentDirectory}"
    )

    if (NOT "${GRAPHICS_ENGINE_CURRENT_TEST}" STREQUAL "")
        list(APPEND testLabels
            "test:${GRAPHICS_ENGINE_CURRENT_TEST}"
        )
    endif ()

    set_tests_properties(
        ${GRAPHICS_ENGINE_CURRENT_TARGET}
        PROPERTIES
        LABELS "${testLabels}"
    )
endmacro()

macro(TEST_MODULE name)
    TEST_SUITE(${name})

    if (TARGET "GraphicsEngine::${name}")
        PRIVATE_DEPENDS(${name})
    endif ()

    TEST(${name})
endmacro()

macro(TEST_SUBMODULE)
    TEST()
endmacro()


macro(TEST_TARGET name label)
    if (NOT GRAPHICS_ENGINE_BUILD_TESTS)
        message(FATAL_ERROR
            "TEST_TARGET: tests are disabled"
        )
    endif ()

    if (TARGET "${name}")
        message(FATAL_ERROR
            "TEST_TARGET: target '${name}' already exists"
        )
    endif ()

    add_custom_target(
        ${name}
        COMMAND
        ${CMAKE_CTEST_COMMAND}
        --test-dir "${CMAKE_BINARY_DIR}"
        --quiet
        --output-on-failure
        --label-regex "${label}"
        DEPENDS
        ${ARGN}
        USES_TERMINAL
    )
endmacro()
