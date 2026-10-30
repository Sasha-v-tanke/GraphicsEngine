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

Admission requires a running Application. This first layer drains all accepted jobs during Stop, including
queued jobs; a result remains accessible after Stop or runtime destruction. Destroying a Job neither cancels
nor detaches its work from runtime shutdown. User-job cancellation/scope and coroutines are separate stages
(IMPROVE-0038, FEATURE-0076, FEATURE-0077).

Captures must own their data or keep borrowed objects alive until completion. There is no implicit mutable
World access or main-thread affinity. A worker must not wait for another job on the same pool, call runtime
Stop, or depend on application-thread actions that cannot run during shutdown. Use short compute steps and
nonblocking result polling; cancellation and coroutine suspension will extend this contract separately.
