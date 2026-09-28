MODULE(Graphics)

API(
    completion_point.h
    frame_submission.h
    graphics.h
    graphics_capabilities.h
    graphics_config.h
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
