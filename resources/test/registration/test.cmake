TEST_SUITE(ResourceRegistrationFault)
SOURCES(allocation.cpp)
PRIVATE_DEPENDS(Resources)
TEST_LABELS(small cpp resources)
set_tests_properties(${GRAPHICS_ENGINE_CURRENT_TARGET} PROPERTIES TIMEOUT 30)

if (GRAPHICS_ENGINE_ENABLE_ASAN)
    target_compile_definitions(
        ${GRAPHICS_ENGINE_CURRENT_TARGET}
        PRIVATE
        GRAPHICS_ENGINE_RESOURCE_FAULT_ASAN=1
    )
endif ()
