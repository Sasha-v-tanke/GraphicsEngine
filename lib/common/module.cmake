MODULE(Common)

API(
    change_tracker.h
    error/assert.h
    error/error.h
    error/exception.h
    resource_use_record.h
    wrapper/non_copyable.h
    wrapper/non_transferable.h
)

SOURCES(change_tracker.cpp)

RECURSE(
    error
    wrapper
    test
)
