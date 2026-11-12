TEST_SUITE(ResourceRetirement)
SOURCES(retirement.cpp)
PRIVATE_DEPENDS(Resources)
TEST_LABELS(small cpp resources)
# Isolate reentrant destructor regressions: a broken implementation can self-deadlock.
set_tests_properties(${GRAPHICS_ENGINE_CURRENT_TARGET} PROPERTIES TIMEOUT 20)
