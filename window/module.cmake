MODULE(Window)

API(
    window.h
    window_config.h
    window_size.h
    window_type.h
)

SOURCES(
    window.cpp
)

PRIVATE_DEPENDS(
    Common
)

RECURSE(
    engine
    test
)
