TEST_SUITE(ThreadDag)

SOURCES(
    cancellation.cpp
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
