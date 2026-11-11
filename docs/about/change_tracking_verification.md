# ChangeTracker acceptance evidence

Recorded on 2026-11-11 for source commit
[`cc53006906591e6a1bfbd24c97f055516093659e`](https://github.com/Sasha-v-tanke/GraphicsEngine/commit/cc53006906591e6a1bfbd24c97f055516093659e).
Production code is unchanged from [PR #110](https://github.com/Sasha-v-tanke/GraphicsEngine/pull/110),
squash commit `d87826581f37450adcff37edc4ade546d4fdde35`.

## Measured heap requests

`EcsRunFault.TrackingHeapIsBoundedAndPinnedSnapshotsReleaseTheirAllocations`
intercepts actual normal C++ allocation/deallocation requests on the publisher thread.
It includes map/vector/deque storage and shared ownership control blocks. A fixed
stack table tracks live pointers, requested sizes and peak bytes without allocating
inside the probe; overflow fails the test. These are allocation-request bytes, not
logical record counts or process RSS. Allocator bookkeeping/rounding and OS runtime
allocations are outside this metric.

The workload uses four domains, four retained history epochs, 2,048 history-record
budget, 64 chunk/entity versions per domain and 32 entity marks per domain/epoch.
After warmup it retains eight publication snapshots, releases them, then publishes
128 more epochs to check steady-state growth. It finally destroys the tracker.
Normal-profile Apple Clang 21.0.0, macOS arm64, five repetitions:

| Measurement | Allocation-request bytes, each repetition |
| --- | ---: |
| Full four-epoch history, no consumer pins | 63,256 |
| Eight pinned publication snapshots | 360,560 |
| After releasing consumer snapshots | 63,256 |
| Maximum steady state over 128 further publications | 63,256 |
| Peak including transient publication allocations | 364,976 |
| After tracker and snapshots are destroyed | 0 |

All five repetitions exited 0. The test asserts bounded live/peak heap budgets,
release of old snapshot allocations, no steady-state growth beyond a 4 KiB allowance,
and no outstanding tracked allocations at destruction. It runs in normal Small/Medium
profiles. The existing ASan executable owns its delete interceptor; this measurement
case is excluded there while all existing allocation-failure tests remain enabled.

```sh
cmake --preset ci-lite -B /private/tmp/tracking-memory
cmake --build /private/tmp/tracking-memory --target GraphicsEngineEcsRunFaultTests -j4
/private/tmp/tracking-memory/GraphicsEngineEcsRunFaultTests \
  --gtest_filter='EcsRunFault.TrackingHeap*' --gtest_repeat=5
```

## TSan execution

Homebrew Clang 23.1.1, macOS arm64; ThreadSanitizer instrumentation enabled for both
the libraries and test executables. Both instrumented test runs exited 0; reproduce with:

```sh
cmake --preset ci-lite -B /private/tmp/tracking-tsan \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
  -DCMAKE_CXX_FLAGS=-fsanitize=thread -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread \
  -DCMAKE_AR=/usr/bin/ar -DCMAKE_RANLIB=/usr/bin/ranlib
cmake --build /private/tmp/tracking-tsan \
  --target GraphicsEngineCommonTests GraphicsEngineEcsTests -j4
TSAN_OPTIONS=halt_on_error=1 /private/tmp/tracking-tsan/GraphicsEngineCommonTests \
  --gtest_filter='ChangeTracker.*' --gtest_repeat=20
TSAN_OPTIONS=halt_on_error=1 /private/tmp/tracking-tsan/GraphicsEngineEcsTests \
  --gtest_filter='EcsChanges.*:EcsScoped.*' --gtest_repeat=20
```

Results: 7 ChangeTracker tests × 20 repetitions and 22 ECS/scoped tests × 20
repetitions passed, with no ThreadSanitizer diagnostics. Coverage includes concurrent
snapshot readers/publication/invalidation, disjoint scoped writers, legacy writes,
failure/cancellation and duplicate READ/WRITE declaration orders. This is a local
TSan run, separate from the GitHub ASan/UBSan profile.

The normal Common/ECS/EcsRunFault CTest profile passed 3/3; the ASan/UBSan
EcsRunFault profile passed 1/1. The original production HEAD `01fd4938` also passed
the separate [Heavy/macOS run](https://github.com/Sasha-v-tanke/GraphicsEngine/actions/runs/43072229342);
the Heavy job skipped on the ordinary PR workflow is not that dispatch result.
