MODULE(Math)

API(
    camera.h
    matrix.h
    quaternion.h
    transform.h
    vector.h
)

SOURCES(
    camera.cpp
    matrix.cpp
    quaternion.cpp
    transform.cpp
    vector.cpp
    internal/glm_conversion.cpp
)

PRIVATE_DEPENDS(
    GLM
)

RECURSE(
    test
)
