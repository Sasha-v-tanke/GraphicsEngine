MODULE(Application)

API(
    application.h
    application_config.h
)

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
