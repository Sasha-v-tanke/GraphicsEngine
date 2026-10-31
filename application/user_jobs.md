# User jobs

A subclass starts independent compute work with protected `RunAsync(F)`. It immediately submits to the
Engine-owned TaskSystem and returns a move-only `NCommon::Job<T>`. No frame slot, arena, dependency or
checkpoint is attached, including when called from `OnUpdate`. Jobs share worker capacity with frames;
this is not a guarantee of frame latency or preemption.

```cpp
m_job = RunAsync([input = std::move(input)] { return Compute(input); });
// In a later update:
if (auto result = m_job.TryTake()) {
    if (*result) {
        Use(std::move(result->value()));
    } else {
        Report(result->error());
    }
    // Do not poll this consumed handle again.
}
```

`IsReady()` is a nonblocking readiness query and returns false on invalid, moved-from or consumed handles.
`TryTake()` returns nullopt while pending, then one `expected<T, ErrorInfo>`. Further consumption throws
`INVALID_STATE`. The same handle must not be consumed or moved concurrently. If moving a result into the
consumer throws, the handle is still consumed. `void`, move-only values and move-only callables are supported;
reference results are rejected. The stored callable is invoked once as an rvalue on a compute worker.

Worker exceptions become result errors: project exceptions retain their code/message, allocation failures
use `OUT_OF_MEMORY`, other exceptions use `UNKNOWN`. Admission/allocation/callable-construction failures before
submission throw synchronously. Job failure does not fail the frame pipeline.

Admission requires a running Application. `RequestCancel()` asynchronously requests cancellation and does
not execute user `stop_callback` functions on its caller. Invalid/moved/consumed handles throw `INVALID_STATE`.
Queued cancellation claims the job before worker execution and publishes `CANCELLED` without needing a free
compute worker. Running work receives a cooperative `std::stop_token` when the callable accepts one; the token
overload is preferred when both signatures are invocable. There is no forced interruption of C++ code.

Cancellation accepted before finalization replaces a successful result with `CANCELLED`; a real worker failure
is preserved. A cancellation arriving after finalization cannot change the result. `TryTake()` consumes cancellation
as an `expected` error with `EError::CANCELLED`, just like any other terminal outcome.

Stop closes admission, cancels queued jobs, requests cooperative stop on running jobs, drains the existing
executor and joins cancellation finalization before runtime owners are destroyed. Each Run creates a fresh scope;
old handles only refer to their retained results. Destroying a Job neither cancels nor detaches its work. A failed
job whose result is abandoned emits one `GraphicsEngine unobserved user job failure` diagnostic to stderr, including
its code/message; observed failures and cancellation do not emit this diagnostic. Abandonment after runtime destruction
still reports. Failure does not latch Engine's frame error state.

The runtime owns one cancellation/retirement dispatcher thread per active Run. It never invokes compute bodies:
these remain on TaskSystem workers. This keeps RequestCancel asynchronous even when a stop callback blocks, and
allows cancellation with WorkerCount=1. Callbacks, capture destructors and result destructors execute outside internal
Engine/scope/task locks. A callback registered after its token is already stopped can execute inline on the registering thread, as specified
by std::stop_callback. Stop callbacks must remain short and must not wait for other cancellation callbacks or scope
shutdown: they serialize on this dispatcher. Arbitrary blocking user callbacks can delay shutdown.

Captures must own their data or keep borrowed objects alive until completion. There is no implicit mutable
World access or main-thread affinity. A worker or cancellation callback must not call runtime Stop or wait for
work on its own executor/dispatcher. Owners captured by reference must outlive drain; subclass members must not be
destroyed while jobs still use them. Coroutine ownership and suspension remain FEATURE-0076/FEATURE-0077.
