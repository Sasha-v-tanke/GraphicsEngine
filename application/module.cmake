MODULE(Application)

SOURCES(
    application.cpp
)

PUBLIC_DEPENDS(
    Window
)

PRIVATE_DEPENDS(
    Engine
)

RECURSE(
    runtime
    test
)
