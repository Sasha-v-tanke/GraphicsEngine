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
    Resources
    Graphics
    Common
    Math
    Ecs
    Scene
    Renderer
)

PRIVATE_DEPENDS(
    Engine
)

RECURSE(
    runtime
    test
)
