MODULE(Logging)

API(
    logger.h
)

PUBLIC_DEPENDS(
    Common
)

SOURCES(
    logger.cpp
)

RECURSE(
    test
)
