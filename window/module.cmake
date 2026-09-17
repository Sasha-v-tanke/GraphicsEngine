MODULE(Window)

SOURCES(
    window.cpp
)

PRIVATE_DEPENDS(
    Common
)

RECURSE(
    internal
    test
)
