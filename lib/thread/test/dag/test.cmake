TEST_SUITE(ThreadDag)

SOURCES(
    cancellation.cpp
    validation.cpp
)

PRIVATE_DEPENDS(
    Thread
    Common
)

set_tests_properties(${GRAPHICS_ENGINE_CURRENT_TARGET} PROPERTIES DISABLED TRUE)

foreach(caseName IN ITEMS
    TransitiveAndNestedDiamonds
    FailureCancelsSharedDescendants
    SharedFanInPreservesUnaffectedBacklinks
    ConcurrentCompletionAndCancellation
    ShutdownDuringFailureCascade
)
    add_test(NAME GraphicsEngineThreadDag${caseName}
        COMMAND ${GRAPHICS_ENGINE_CURRENT_TARGET} "--gtest_filter=TaskDag.${caseName}"
    )
    set_tests_properties(GraphicsEngineThreadDag${caseName} PROPERTIES
        TIMEOUT 60
        LABELS "small;cpp;thread;task;test:TaskDag"
    )
endforeach()

foreach(caseName IN ITEMS DeepChain200k WideFanIn100k)
    add_test(NAME GraphicsEngineThreadDag${caseName}
        COMMAND ${GRAPHICS_ENGINE_CURRENT_TARGET} "--gtest_filter=TaskDag.${caseName}"
    )
    set_tests_properties(GraphicsEngineThreadDag${caseName} PROPERTIES
        TIMEOUT 120
        LABELS "heavy;cpp;thread;task;stress;test:TaskDag"
        SKIP_REGULAR_EXPRESSION "\\[  SKIPPED \\]"
    )
endforeach()

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_test(NAME GraphicsEngineThreadDagValidation
        COMMAND ${GRAPHICS_ENGINE_CURRENT_TARGET} "--gtest_filter=DebugDag.*"
    )
    set_tests_properties(GraphicsEngineThreadDagValidation PROPERTIES
        TIMEOUT 60
        LABELS "small;cpp;thread;task;test:DebugDag"
    )
endif ()
