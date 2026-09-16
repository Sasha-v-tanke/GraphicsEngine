include_guard(GLOBAL)


# =============================================================================
# Internal helpers
# =============================================================================

macro(_GRAPHICS_ENGINE_REQUIRE_CONTEXT commandName)
    if (NOT DEFINED GRAPHICS_ENGINE_CURRENT_TARGET)
        message(FATAL_ERROR
            "${commandName}: no active module context"
        )
    endif ()
endmacro()


macro(_GRAPHICS_ENGINE_ADD_LOCAL_HEADERS)
    _GRAPHICS_ENGINE_REQUIRE_CONTEXT(
        "_GRAPHICS_ENGINE_ADD_LOCAL_HEADERS"
    )

    file(GLOB localHeaders CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/*.h"
    )

    if (localHeaders)
        target_sources(
            ${GRAPHICS_ENGINE_CURRENT_TARGET}
            PRIVATE
            ${localHeaders}
        )
    endif ()
endmacro()


macro(_GRAPHICS_ENGINE_RESOLVE_DEPENDENCY output dependency)
    if ("${dependency}" MATCHES "::")
        set(
            ${output}
            "${dependency}"
        )
    elseif (TARGET "${dependency}")
        set(
            ${output}
            "${dependency}"
        )
    else ()
        set(
            ${output}
            "GraphicsEngine::${dependency}"
        )
    endif ()
endmacro()


macro(_GRAPHICS_ENGINE_FIND_ENTRY output directory)
    set(entryFiles)

    foreach (entryName IN ITEMS
        module.cmake
        submodule.cmake
        test.cmake
        test_module.cmake
        test_submodule.cmake
        benchmark.cmake
    )
        if (EXISTS "${directory}/${entryName}")
            list(APPEND entryFiles
                "${directory}/${entryName}"
            )
        endif ()
    endforeach ()

    list(LENGTH entryFiles entryCount)

    if (entryCount EQUAL 0)
        message(FATAL_ERROR
            "RECURSE: no module entry file found in '${directory}'"
        )
    endif ()

    if (entryCount GREATER 1)
        message(FATAL_ERROR
            "RECURSE: multiple module entry files found in '${directory}'"
        )
    endif ()

    list(GET entryFiles 0 ${output})
endmacro()


# =============================================================================
# Production modules
# =============================================================================

macro(MODULE name)
    set(
        moduleTarget
        "GraphicsEngine${name}"
    )

    if (TARGET "${moduleTarget}")
        message(FATAL_ERROR
            "MODULE: target '${moduleTarget}' already exists"
        )
    endif ()

    add_library(
        ${moduleTarget}
    )

    _GRAPHICS_ENGINE_APPLY_PROJECT_OPTIONS(
        ${moduleTarget}
    )

    add_library(
        GraphicsEngine::${name}
        ALIAS
        ${moduleTarget}
    )

    target_compile_features(
        ${moduleTarget}
        PUBLIC
        cxx_std_23
    )

    if (GRAPHICS_ENGINE_BUILD_TESTS)
        set(
            testTarget
            "GraphicsEngine${name}Tests"
        )

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
            GraphicsEngine::${name}
        )

        add_test(
            NAME ${testTarget}
            COMMAND ${testTarget}
        )
    endif ()

    set(
        GRAPHICS_ENGINE_CURRENT_CONTEXT
        "MODULE"
    )

    set(
        GRAPHICS_ENGINE_CURRENT_MODULE
        "${name}"
    )

    set(
        GRAPHICS_ENGINE_CURRENT_TARGET
        "${moduleTarget}"
    )

    _GRAPHICS_ENGINE_ADD_LOCAL_HEADERS()
endmacro()


macro(SUBMODULE)
    _GRAPHICS_ENGINE_REQUIRE_CONTEXT(
        "SUBMODULE"
    )

    if (NOT GRAPHICS_ENGINE_CURRENT_CONTEXT STREQUAL "MODULE")
        message(FATAL_ERROR
            "SUBMODULE: current context is not a production module"
        )
    endif ()

    _GRAPHICS_ENGINE_ADD_LOCAL_HEADERS()
endmacro()


# =============================================================================
# Test modules
# =============================================================================

macro(TEST_MODULE name)
    if (NOT GRAPHICS_ENGINE_BUILD_TESTS)
        message(FATAL_ERROR
            "TEST_MODULE: tests are disabled"
        )
    endif ()

    set(
        testTarget
        "GraphicsEngine${name}Tests"
    )

    if (NOT TARGET "${testTarget}")
        message(FATAL_ERROR
            "TEST_MODULE: module '${name}' does not exist"
        )
    endif ()

    set(
        GRAPHICS_ENGINE_CURRENT_CONTEXT
        "TEST"
    )

    set(
        GRAPHICS_ENGINE_CURRENT_MODULE
        "${name}"
    )

    set(
        GRAPHICS_ENGINE_CURRENT_TARGET
        "${testTarget}"
    )

    _GRAPHICS_ENGINE_ADD_LOCAL_HEADERS()
endmacro()


macro(TEST_SUBMODULE name)
    if (NOT GRAPHICS_ENGINE_BUILD_TESTS)
        message(FATAL_ERROR
            "TEST_SUBMODULE: tests are disabled"
        )
    endif ()

    set(
        testTarget
        "GraphicsEngine${name}Tests"
    )

    if (NOT TARGET "${testTarget}")
        message(FATAL_ERROR
            "TEST_SUBMODULE: module '${name}' does not exist"
        )
    endif ()

    set(
        GRAPHICS_ENGINE_CURRENT_CONTEXT
        "TEST"
    )

    set(
        GRAPHICS_ENGINE_CURRENT_MODULE
        "${name}"
    )

    set(
        GRAPHICS_ENGINE_CURRENT_TARGET
        "${testTarget}"
    )

    _GRAPHICS_ENGINE_ADD_LOCAL_HEADERS()
endmacro()


macro(TEST name)
    if (NOT GRAPHICS_ENGINE_BUILD_TESTS)
        message(FATAL_ERROR
            "TEST: tests are disabled"
        )
    endif ()

    set(
        testTarget
        "GraphicsEngine${name}Tests"
    )

    if (TARGET "${testTarget}")
        message(FATAL_ERROR
            "TEST: target '${testTarget}' already exists"
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

    add_test(
        NAME ${testTarget}
        COMMAND ${testTarget}
    )

    set(
        GRAPHICS_ENGINE_CURRENT_CONTEXT
        "TEST"
    )

    set(
        GRAPHICS_ENGINE_CURRENT_MODULE
        "${name}"
    )

    set(
        GRAPHICS_ENGINE_CURRENT_TARGET
        "${testTarget}"
    )

    _GRAPHICS_ENGINE_ADD_LOCAL_HEADERS()
endmacro()


macro(TEST_LABELS)
    _GRAPHICS_ENGINE_REQUIRE_CONTEXT(
        "TEST_LABELS"
    )

    if (NOT GRAPHICS_ENGINE_CURRENT_CONTEXT STREQUAL "TEST")
        message(FATAL_ERROR
            "TEST_LABELS: current context is not a test"
        )
    endif ()

    set_tests_properties(
        ${GRAPHICS_ENGINE_CURRENT_TARGET}
        PROPERTIES
        LABELS "${ARGN}"
    )
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


# =============================================================================
# Sources
# =============================================================================

macro(SOURCES)
    _GRAPHICS_ENGINE_REQUIRE_CONTEXT(
        "SOURCES"
    )

    foreach (file IN ITEMS ${ARGN})
        target_sources(
            ${GRAPHICS_ENGINE_CURRENT_TARGET}
            PRIVATE
            "${CMAKE_CURRENT_LIST_DIR}/${file}"
        )
    endforeach ()
endmacro()


# =============================================================================
# Dependencies
# =============================================================================

macro(PUBLIC_DEPENDS)
    _GRAPHICS_ENGINE_REQUIRE_CONTEXT(
        "PUBLIC_DEPENDS"
    )

    foreach (dependency IN ITEMS ${ARGN})
        _GRAPHICS_ENGINE_RESOLVE_DEPENDENCY(
            resolvedDependency
            "${dependency}"
        )

        target_link_libraries(
            ${GRAPHICS_ENGINE_CURRENT_TARGET}
            PUBLIC
            ${resolvedDependency}
        )
    endforeach ()
endmacro()


macro(PRIVATE_DEPENDS)
    _GRAPHICS_ENGINE_REQUIRE_CONTEXT(
        "PRIVATE_DEPENDS"
    )

    foreach (dependency IN ITEMS ${ARGN})
        _GRAPHICS_ENGINE_RESOLVE_DEPENDENCY(
            resolvedDependency
            "${dependency}"
        )

        target_link_libraries(
            ${GRAPHICS_ENGINE_CURRENT_TARGET}
            PRIVATE
            ${resolvedDependency}
        )
    endforeach ()
endmacro()


# =============================================================================
# Directory recursion
# =============================================================================

macro(RECURSE)
    foreach (directory IN ITEMS ${ARGN})
        set(
            childDirectory
            "${CMAKE_CURRENT_LIST_DIR}/${directory}"
        )

        if (NOT IS_DIRECTORY "${childDirectory}")
            message(FATAL_ERROR
                "RECURSE: '${childDirectory}' is not a directory"
            )
        endif ()

        _GRAPHICS_ENGINE_FIND_ENTRY(
            entryFile
            "${childDirectory}"
        )

        get_filename_component(
            entryFileName
            "${entryFile}"
            NAME
        )

        if (
            NOT GRAPHICS_ENGINE_BUILD_TESTS
            AND (
            entryFileName STREQUAL "test_module.cmake"
            OR
            entryFileName STREQUAL "test.cmake"
            OR
            entryFileName STREQUAL "test_submodule.cmake"
        )
        )
            continue()
        endif ()

        if (
            NOT GRAPHICS_ENGINE_BUILD_BENCHMARKS
            AND entryFileName STREQUAL "benchmark.cmake"
        )
            continue()
        endif ()

        set(
            savedContext
            "${GRAPHICS_ENGINE_CURRENT_CONTEXT}"
        )

        set(
            savedModule
            "${GRAPHICS_ENGINE_CURRENT_MODULE}"
        )

        set(
            savedTarget
            "${GRAPHICS_ENGINE_CURRENT_TARGET}"
        )

        include(
            "${entryFile}"
        )

        set(
            GRAPHICS_ENGINE_CURRENT_CONTEXT
            "${savedContext}"
        )

        set(
            GRAPHICS_ENGINE_CURRENT_MODULE
            "${savedModule}"
        )

        set(
            GRAPHICS_ENGINE_CURRENT_TARGET
            "${savedTarget}"
        )
    endforeach ()
endmacro()

# =============================================================================
# Benchmark modules
# =============================================================================

macro(BENCHMARK_MODULE)
    if (NOT GRAPHICS_ENGINE_BUILD_BENCHMARKS)
        message(FATAL_ERROR
            "BENCHMARK_MODULE: benchmarks are disabled"
        )
    endif ()

    set(
        benchmarkTarget
        "GraphicsEngineBenchmarks"
    )

    if (TARGET "${benchmarkTarget}")
        message(FATAL_ERROR
            "BENCHMARK_MODULE: target '${benchmarkTarget}' already exists"
        )
    endif ()

    add_executable(
        ${benchmarkTarget}
    )

    _GRAPHICS_ENGINE_APPLY_PROJECT_OPTIONS(
        ${benchmarkTarget}
    )

    target_compile_features(
        ${benchmarkTarget}
        PRIVATE
        cxx_std_23
    )

    target_link_libraries(
        ${benchmarkTarget}
        PRIVATE
        GraphicsEngine::Benchmark
    )

    set(
        GRAPHICS_ENGINE_CURRENT_CONTEXT
        "BENCHMARK"
    )

    set(
        GRAPHICS_ENGINE_CURRENT_TARGET
        "${benchmarkTarget}"
    )

    _GRAPHICS_ENGINE_ADD_LOCAL_HEADERS()
endmacro()

macro(BENCHMARK)
    _GRAPHICS_ENGINE_REQUIRE_CONTEXT(
        "BENCHMARK"
    )

    if (NOT GRAPHICS_ENGINE_CURRENT_CONTEXT STREQUAL "BENCHMARK")
        message(FATAL_ERROR
            "BENCHMARK: current context is not a benchmark"
        )
    endif ()

    _GRAPHICS_ENGINE_ADD_LOCAL_HEADERS()
endmacro()

# =============================================================================
# Samples
# =============================================================================

macro(SAMPLE name)
    set(sampleTarget "GraphicsEngineSample${name}")

    add_executable(
        ${sampleTarget}
    )

    _GRAPHICS_ENGINE_APPLY_PROJECT_OPTIONS(
        ${sampleTarget}
    )

    target_compile_features(
        ${sampleTarget}
        PRIVATE
        cxx_std_23
    )

    set(
        GRAPHICS_ENGINE_CURRENT_CONTEXT
        "SAMPLE"
    )

    set(
        GRAPHICS_ENGINE_CURRENT_TARGET
        "${sampleTarget}"
    )

    _GRAPHICS_ENGINE_ADD_LOCAL_HEADERS()
endmacro()

# =============================================================================
# Library include
# =============================================================================

function(include_external Name)
    include(
        "${CMAKE_CURRENT_LIST_DIR}/${Name}/${Name}.cmake"
    )
endfunction()

# ============================================================================
# Local checks
# ============================================================================

macro(_GRAPHICS_ENGINE_APPLY_PROJECT_OPTIONS target)
    target_link_libraries(
        ${target}
        PRIVATE
        GraphicsEngine::Warnings
        GraphicsEngine::Sanitizers
    )
endmacro()
