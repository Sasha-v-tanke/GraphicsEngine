TEST(Task)

SOURCES(
    task_system_test.cpp
)

set(taskSystemStressTests
    TaskSystem.HandlesConcurrentDependencyCompletionAndDependentPublication
    TaskSystem.HandlesContentionStressWithoutDroppingTasks
    TaskSystem.DoesNotLoseWakeupsAcrossRepeatedIdleSubmissions
    TaskSystem.WaitIdleDoesNotLoseConcurrentLastCompletionWakeup
    TaskSystem.RepeatedCreateCancelCyclesDoNotAccumulateBacklinks
    TaskSystem.HandlesConcurrentPrerequisiteCompletionAndDependentCancellation
    TaskSystem.RunsManySubmitWaitCyclesWithoutRetainingPayloads
)

list(JOIN taskSystemStressTests ":" taskSystemStressFilter)

set_tests_properties(
    ${GRAPHICS_ENGINE_CURRENT_TARGET}
    PROPERTIES
    DISABLED TRUE
)

add_test(
    NAME GraphicsEngineThreadTaskContractTests
    COMMAND ${GRAPHICS_ENGINE_CURRENT_TARGET} "--gtest_filter=TaskSystem.*-${taskSystemStressFilter}"
)

set_tests_properties(
    GraphicsEngineThreadTaskContractTests
    PROPERTIES
    LABELS "small;cpp;thread;task;dir:lib/thread/test/task;test:TaskSystemContract"
)

add_test(
    NAME GraphicsEngineThreadTaskStressTests
    COMMAND ${GRAPHICS_ENGINE_CURRENT_TARGET} "--gtest_filter=${taskSystemStressFilter}"
)

set_tests_properties(
    GraphicsEngineThreadTaskStressTests
    PROPERTIES
    LABELS "heavy;cpp;thread;task;stress;dir:lib/thread/test/task;test:TaskSystemStress"
)
