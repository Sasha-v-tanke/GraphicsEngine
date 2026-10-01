MODULE(Graphics)

API(
    buffer.h
    completion_point.h
    frame_submission.h
    graphics.h
    graphics_capabilities.h
    graphics_config.h
    graphics_pipeline.h
    shader.h
)

SOURCES(
    graphics.cpp
    graphics_pipeline.cpp
    shader.cpp
)

PUBLIC_DEPENDS(
    Common
    Resources
)

RECURSE(
    backend
    test
)

if (GRAPHICS_ENGINE_BUILD_VULKAN)
    RECURSE(
        vulkan
    )
endif ()
