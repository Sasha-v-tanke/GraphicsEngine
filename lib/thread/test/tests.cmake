TEST_SUITE(Thread)

RECURSE(
    task
    dag
    retirement_fault
)

PRIVATE_DEPENDS(
    Thread
    Common
)
