MODULE(Thread)

API(
    task/task_system.h
)

PRIVATE_DEPENDS(
    Common
)

RECURSE(
    task
    test
)
