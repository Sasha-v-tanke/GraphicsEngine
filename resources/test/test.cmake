TEST_MODULE(Resources)

SOURCES(
    resource_observation.cpp
    async_resource_loader.cpp
    resource_io.cpp
    mesh_loader.cpp
    resource_manager_test.cpp
)

TEST_LABELS(
    small
    cpp
    resources
)

PRIVATE_DEPENDS(
    Thread
)

RECURSE(retirement)
