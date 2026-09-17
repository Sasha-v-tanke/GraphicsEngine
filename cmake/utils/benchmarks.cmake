include_guard(GLOBAL)

macro(BENCHMARKS name)
    if (NOT GRAPHICS_ENGINE_BUILD_BENCHMARKS)
        message(FATAL_ERROR
            "BENCHMARKS: benchmarks are disabled"
        )
    endif ()

    set(
        benchmarkTarget
        "GraphicsEngineBenchmarks"
    )

    if (TARGET "${benchmarkTarget}")
        message(FATAL_ERROR
            "BENCHMARKS: target '${benchmarkTarget}' already exists"
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
        "BENCHMARKS"
    )

    set(
        GRAPHICS_ENGINE_CURRENT_BENCHMARK_SUITE
        "${name}"
    )

    set(
        GRAPHICS_ENGINE_CURRENT_TARGET
        "${benchmarkTarget}"
    )

    set(
        GRAPHICS_ENGINE_CURRENT_BENCHMARK
        ""
    )

    _GRAPHICS_ENGINE_ADD_LOCAL_HEADERS()
endmacro()

function(BENCHMARK)
    _GRAPHICS_ENGINE_REQUIRE_CONTEXT(
        "BENCHMARK"
    )

    if (
        NOT GRAPHICS_ENGINE_CURRENT_CONTEXT STREQUAL "BENCHMARKS"
        AND
        NOT GRAPHICS_ENGINE_CURRENT_CONTEXT STREQUAL "BENCHMARK"
    )
        message(FATAL_ERROR
            "BENCHMARK: current context is not a benchmark suite"
        )
    endif ()

    if (ARGC GREATER 1)
        message(FATAL_ERROR
            "BENCHMARK: expected zero or one argument"
        )
    endif ()

    if (ARGC EQUAL 1)
        set(
            GRAPHICS_ENGINE_CURRENT_BENCHMARK
            "${ARGV0}"
        )
    elseif ("${GRAPHICS_ENGINE_CURRENT_BENCHMARK}" STREQUAL "")
        message(FATAL_ERROR
            "BENCHMARK: unnamed benchmark has no active benchmark context"
        )
    endif ()

    set(
        GRAPHICS_ENGINE_CURRENT_CONTEXT
        "BENCHMARK"
    )

    _GRAPHICS_ENGINE_ADD_LOCAL_HEADERS()

    set(
        GRAPHICS_ENGINE_CURRENT_CONTEXT
        "${GRAPHICS_ENGINE_CURRENT_CONTEXT}"
        PARENT_SCOPE
    )

    set(
        GRAPHICS_ENGINE_CURRENT_BENCHMARK
        "${GRAPHICS_ENGINE_CURRENT_BENCHMARK}"
        PARENT_SCOPE
    )
endfunction()

macro(BENCHMARK_LABELS)
    _GRAPHICS_ENGINE_REQUIRE_CONTEXT(
        "BENCHMARK_LABELS"
    )

    if (
        NOT GRAPHICS_ENGINE_CURRENT_CONTEXT STREQUAL "BENCHMARK"
        AND
        NOT GRAPHICS_ENGINE_CURRENT_CONTEXT STREQUAL "BENCHMARKS"
    )
        message(FATAL_ERROR
            "BENCHMARK_LABELS: current context is not a benchmark"
        )
    endif ()

    file(
        RELATIVE_PATH
        currentDirectory
        "${PROJECT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}"
    )

    set(benchmarkLabels
        ${ARGN}
        "dir:${currentDirectory}"
    )

    if (NOT "${GRAPHICS_ENGINE_CURRENT_BENCHMARK}" STREQUAL "")
        list(APPEND benchmarkLabels
            "benchmark:${GRAPHICS_ENGINE_CURRENT_BENCHMARK}"
        )
    endif ()

    set_property(
        TARGET ${GRAPHICS_ENGINE_CURRENT_TARGET}
        APPEND
        PROPERTY GRAPHICS_ENGINE_BENCHMARK_LABELS
        ${benchmarkLabels}
    )
endmacro()

macro(BENCHMARK_MODULE)
    BENCHMARKS(Main)
endmacro()
