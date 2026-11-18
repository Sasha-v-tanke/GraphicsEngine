TEST_SUITE(Common)

SOURCES(
    change_tracker.cpp
    type_registry.cpp
    type_registry_gameplay.cpp
    type_registry_assets.cpp
)

PRIVATE_DEPENDS(
    Common
)

RECURSE(
    error
    wrapper
)

TEST_LABELS(
    small
    cpp
    common
)
