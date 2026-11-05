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

PRIVATE_DEPENDS(Ecs)

RECURSE(
    controller
    runtime
    test
)
