TEST_SUITE(Logging)

SOURCES(
    logger.cpp
    logger_compiletime.cpp
)

PRIVATE_DEPENDS(
    Logging
)

TEST_LABELS(
    small
    cpp
    logging
)
