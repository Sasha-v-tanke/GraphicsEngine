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
