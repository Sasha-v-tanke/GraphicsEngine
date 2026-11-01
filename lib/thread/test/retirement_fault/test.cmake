TEST_SUITE(ThreadRetirementFault)

SOURCES(
    allocation.cpp
)

PRIVATE_DEPENDS(
    Thread
    Common
)

TEST_LABELS(small cpp thread task)

set_tests_properties(
    ${GRAPHICS_ENGINE_CURRENT_TARGET}
    PROPERTIES
    TIMEOUT 45
)

if (GRAPHICS_ENGINE_ENABLE_ASAN)
    target_compile_definitions(
        ${GRAPHICS_ENGINE_CURRENT_TARGET}
        PRIVATE
        GRAPHICS_ENGINE_TASK_FAULT_ASAN=1
    )
endif ()
