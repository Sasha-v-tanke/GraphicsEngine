MODULE(Engine)

SOURCES(
    engine.cpp
)

PRIVATE_DEPENDS(
    Common
    Thread
)

RECURSE(
    controller
    test
)
