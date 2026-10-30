MODULE(Engine)

API(
    engine.h
    engine_config.h
)

SOURCES(
    engine.cpp
)

PUBLIC_DEPENDS(
    Common
    Thread
)

RECURSE(
    controller
    runtime
    test
)
