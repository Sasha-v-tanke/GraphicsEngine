MODULE(Common)

API(
    error/assert.h
    error/error.h
    error/exception.h
    wrapper/non_copyable.h
    wrapper/non_transferable.h
)

RECURSE(
    error
    wrapper
    test
)
