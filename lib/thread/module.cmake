MODULE(Thread)

API(
    job.h
    job_control.h
    task/task_system.h
)

PUBLIC_DEPENDS(
    Common
)

RECURSE(
    task
    test
)
