MODULE(Thread)

API(
    task/task_system.h
)

PUBLIC_DEPENDS(
    Common
)

RECURSE(
    task
    test
)
