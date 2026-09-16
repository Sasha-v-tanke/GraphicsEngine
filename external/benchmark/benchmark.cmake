find_package(
    benchmark
    CONFIG
    REQUIRED
)

add_library(
    GraphicsEngineExternalBenchmark
    INTERFACE
)

add_library(
    GraphicsEngine::Benchmark
    ALIAS
    GraphicsEngineExternalBenchmark
)

target_link_libraries(
    GraphicsEngineExternalBenchmark
    INTERFACE
    benchmark::benchmark_main
)
