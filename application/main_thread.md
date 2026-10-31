# Synchronous main-thread actions

`Application::RunOnMainThread(F&&)` is a protected, void-returning operation for short application-thread actions.
Only invocable, move-constructible callables returning exactly void are accepted, including move-only captures.
On the application thread during Run, execution is immediate; nested calls do not enqueue or wait.
On another thread, execution is queued and the caller waits on the command completion, without TaskSystem::Wait
or executing other CPU tasks. A normal return confirms completion unless shutdown skipped the action.

Before the first Run, calls throw INVALID_STATE. During/after shutdown, unstarted actions silently skip and
all callers wake. Running actions finish normally and cannot be preempted. Queued exceptions propagate to the
waiting caller without failing the main loop; direct exceptions propagate normally. Each Run opens a fresh
generation so old admitted work cannot execute during a restart.

The template borrows the caller's callable until synchronous completion; the queued trampoline never outlives
that wait. Keep the Application and anything referenced by the callable alive through the call. Producers must
finish before Application destruction; a subclass must join producers before destroying captured subclass state.
Do not hold runtime, ECS or user locks needed by the main action across the wait. Affinity grants no additional
permission to access World. Commands continue to dispatch under frame backpressure, including WorkerCount=1.
Stop closes admission and skips pending commands before joining workers, preventing an engine-owned circular wait.

Window events run before each bounded command batch. FIFO follows accepted queue insertion order; frame-origin
priorities/fairness belong to IMPROVE-0037. The application thread remains outside the compute worker pool.
