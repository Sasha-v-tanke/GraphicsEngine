MODULE(Common)

API(
    change_tracker.h
    metadata.h
    error/assert.h
    error/error.h
    error/exception.h
    resource_use_record.h
    type_registry.h
    wrapper/non_copyable.h
    wrapper/non_transferable.h
)

SOURCES(
    change_tracker.cpp
    type_registry.cpp
)

RECURSE(
    error
    wrapper
    test
)
