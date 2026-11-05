MODULE(Thread)

API(
    job.h
    job_control.h
    user_task.h
    task/task_system.h
    task/frame_origin.h
)

PUBLIC_DEPENDS(
    Common
)

RECURSE(
    task
    test
)
