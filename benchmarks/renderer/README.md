# Renderer preparation tuning

Build with `cmake --preset benchmark` and `cmake --build --preset benchmark`, then run:

```sh
cmake-build-benchmark/GraphicsEngineBenchmarks --benchmark_filter=BM_RendererPreparation \
  --benchmark_min_time=0.05s --benchmark_repetitions=3 --benchmark_report_aggregates_only=true
```

The sweep uses two views, shared mesh/material, four persistent workers and chunk sizes 64/256/1024/4096. Chunk size 0
is the synchronous reference. Wall time includes task submission, join, sorting, batching and per-iteration arena lifetime;
scene setup and worker construction are outside the timed loop. It measures the current conservative visibility path,
not future culling or expensive material processing.

Local Release sample on macOS arm64, 2026-10-22, median of three repetitions (microseconds):

| Objects | Serial | Chunk 64 | Chunk 256 | Chunk 1024 | Chunk 4096 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1024 | 19.6 | 80.2 | 49.8 | 53.0 | 36.7 |
| 16384 | 311.4 | 966.1 | 475.9 | 382.4 | 379.3 |

4096 avoids excessive scheduling overhead and was the best scheduled choice in this sample. Serial preparation was
still faster for this inexpensive workload; these results do not establish a general parallel speedup or a universal
threshold. Retune for target hardware, views, scene/resource distribution and visibility/material work. The chunk size
remains caller-selectable, and the synchronous path remains available.
