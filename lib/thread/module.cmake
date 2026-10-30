MODULE(Thread)

API(
    job.h
    task/task_system.h
)

PUBLIC_DEPENDS(
    Common
)

RECURSE(
    task
    test
)
