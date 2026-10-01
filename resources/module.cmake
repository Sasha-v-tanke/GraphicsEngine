MODULE(Resources)

API(
    resource_handle.h
    resource_identity.h
    resource_manager.h
    resource_state.h
    shader_artifact.h
    shader_artifact_loader.h
)

SOURCES(
    resource_manager.cpp
    shader_artifact_loader.cpp
)

PUBLIC_DEPENDS(
    Common
)

RECURSE(
    test
)
