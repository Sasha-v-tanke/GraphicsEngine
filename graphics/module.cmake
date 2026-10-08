MODULE(Graphics)

API(
    buffer.h
    completion_point.h
    frame_submission.h
    graphics.h
    graphics_capabilities.h
    graphics_config.h
    image.h
)

SOURCES(
    graphics.cpp
)

PUBLIC_DEPENDS(
    Common
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
