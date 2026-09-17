include_guard(GLOBAL)

macro(_GRAPHICS_ENGINE_FIND_ENTRY output directory)
    set(entryFiles)

    foreach (entryName IN ITEMS
        module.cmake
        submodule.cmake
        test.cmake
        tests.cmake
        test_module.cmake
        test_submodule.cmake
        benchmark.cmake
        benchmarks.cmake
        sample.cmake
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
            entryFileName STREQUAL "tests.cmake"
            OR
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
            AND (
                entryFileName STREQUAL "benchmarks.cmake"
                OR
                entryFileName STREQUAL "benchmark.cmake"
            )
        )
            continue()
        endif ()

        if (
            NOT GRAPHICS_ENGINE_BUILD_SAMPLES
            AND entryFileName STREQUAL "sample.cmake"
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

        set(
            savedTest
            "${GRAPHICS_ENGINE_CURRENT_TEST}"
        )

        set(
            savedBenchmark
            "${GRAPHICS_ENGINE_CURRENT_BENCHMARK}"
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

        set(
            GRAPHICS_ENGINE_CURRENT_TEST
            "${savedTest}"
        )

        set(
            GRAPHICS_ENGINE_CURRENT_BENCHMARK
            "${savedBenchmark}"
        )
    endforeach ()
endmacro()
