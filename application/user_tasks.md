# UserTask and Spawn

`NCommon::UserTask<T>` is a lazy, move-only C++23 coroutine. Creating it allocates/constructs its frame and
moves/copies value parameters, but does not execute its body. `Application::Spawn(UserTask<T>&&)` is a protected
facade returning `Job<T>` and immediately submitting the initial resume to the existing TaskSystem.

```cpp
NCommon::UserTask<std::unique_ptr<int>> MakeValue(std::unique_ptr<int> input) {
    const std::stop_token stop = co_await NCommon::UserTaskStopToken{};
    if (stop.stop_requested()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::CANCELLED, "Value computation cancelled");
    }
    co_return std::move(input);
}

// Inside an Application subclass:
m_valueJob = Spawn(MakeValue(std::make_unique<int>(42)));
// Consume through the same IsReady/TryTake contract as RunAsync.
```

`T`, `void`, move-only results and value parameters are supported. Reference result types are rejected.
`unhandled_exception` retains the original exception until the runtime translates it to the usual Job error.
Creation/allocation failures occur synchronously; invalid, moved-from or already spawned tasks throw
`INVALID_STATE`. A valid task is consumed by an attempted Spawn even if allocation/admission subsequently fails.
Destroying or replacing an unspawned task destroys its initial-suspended frame without executing its body.

After Spawn, the existing runtime-owned UserJobScope entry owns the coroutine frame through its private execution
payload. There is no second compute executor or detached coroutine lifetime. The frame remains owned while running,
then is destroyed at final suspension before the Job result becomes visible. The typed Job retains its value/error
independently of the frame and Engine. Dropping Job does not cancel work; unobserved failures use the existing diagnostic.

Queued cancellation destroys the initial-suspended frame without resuming it. A running coroutine receives the Job's
cooperative stop token through the ready-only `co_await UserTaskStopToken{}` query. Running C++ code is never forcibly
interrupted or destroyed. Cancellation before finalization replaces successful results with CANCELLED; a real failure
remains observable. Stop closes admission, cancels queued work and drains running work before destroying runtime owners.
Each Run uses its own scope; old handles cannot start work in a new Run.

This foundation supports coroutines that run through to final suspension. Job awaiting and suspended continuation
scheduling are FEATURE-0077: other explicit awaiters are rejected at compile time until their ownership/resume handshake
is implemented. The stop-token query is always ready and never blocks or suspends a worker. No worker wait/poll loop is
introduced by Spawn. Long user computations still consume worker CPU; jobs/coroutines have no frame dependencies,
slots or arenas even when spawned in OnUpdate. Publication to World/main-thread state still requires the appropriate
owner's access/command protocol; Spawn grants neither ECS access nor main-thread affinity.

Pass owned data as value parameters. References, member `this` and coroutine-lambda capture closures remain borrowed:
they must outlive completion. In particular, invoking a temporary capturing coroutine lambda does not transfer its
closure into the coroutine frame. Prefer free/static functions with owned value parameters or a member function whose
owner remains alive until Run/Stop has drained. The implementation follows the C++ coroutine initial/final-suspension
and parameter-lifetime rules: https://eel.is/c++draft/dcl.fct.def.coroutine.
