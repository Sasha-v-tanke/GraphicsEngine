MODULE(Engine)

SOURCES(
    engine.cpp
)

PUBLIC_DEPENDS(
    Common
)

PRIVATE_DEPENDS(
    Thread
)

RECURSE(
    controller
    runtime
    test
)
