MODULE(Resources)

API(
    resource_handle.h
    resource_identity.h
    resource_manager.h
    resource_state.h
)

SOURCES(
    resource_manager.cpp
)

PUBLIC_DEPENDS(
    Common
)

RECURSE(
    test
)
