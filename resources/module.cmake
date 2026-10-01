MODULE(Resources)

API(
    image_data.h
    image_loader.h
    resource_handle.h
    resource_identity.h
    resource_manager.h
    resource_state.h
    shader_artifact.h
    shader_artifact_loader.h
)

SOURCES(
    image_loader.cpp
    resource_manager.cpp
    shader_artifact_loader.cpp
)

PUBLIC_DEPENDS(
    Common
)

PRIVATE_DEPENDS(
    StbImage
)

RECURSE(
    test
)
