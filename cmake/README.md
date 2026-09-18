# CMake Utilities

`utils.cmake` provides a small DSL for declaring GraphicsEngine modules, tests, benchmarks, samples, sources, and
dependencies.

The public API is:

```cmake
MODULE(name)
SUBMODULE()

TESTS(name)
TEST(name)
TEST()

BENCHMARKS(name)
BENCHMARK(name)
BENCHMARK()

SAMPLE(name)

SOURCES(...)
PUBLIC_DEPENDS(...)
PRIVATE_DEPENDS(...)

TEST_LABELS(...)
BENCHMARK_LABELS(...)

RECURSE(...)
```

Tests and benchmarks do not infer production dependencies from their names. Link every required module explicitly with
`PRIVATE_DEPENDS(...)`.

## Entry Files

Directories participating in the build use one entry file:

```text
module.cmake
submodule.cmake
tests.cmake
test.cmake
benchmarks.cmake
benchmark.cmake
sample.cmake
```

`RECURSE()` automatically finds the entry file. A directory must contain exactly one supported entry file.

## Production Modules

Create a module:

```cmake
MODULE(Window)

SOURCES(
    window.cpp
)

PRIVATE_DEPENDS(
    Common
)

RECURSE(
    internal
    test
)
```

This creates:

```text
GraphicsEngineWindow
GraphicsEngine::Window
```

Production modules are static libraries. This keeps `PRIVATE_DEPENDS` semantics stable regardless of
`BUILD_SHARED_LIBS`: private dependencies remain part of the installed consumer link closure, but they do not extend the
public header API.

Extend an existing module from a child directory:

```cmake
SUBMODULE()

SOURCES(
    engine.cpp
)
```

`SUBMODULE()` does not create another target. It adds sources and local headers to the current module target.

## Tests

Create a test suite:

```cmake
TESTS(Window)

TEST(Window)

SOURCES(
    window_test.cpp
)

PRIVATE_DEPENDS(
    Window
)

TEST_LABELS(
    small
    cpp
    window
)
```

This creates:

```text
GraphicsEngineWindowTests
GraphicsEngine::WindowTests
```

`TESTS(name)` creates the executable. `TEST(name)` starts a logical test group inside the current suite. `TEST()` from a
child directory continues the active test group:

```cmake
TEST()

SOURCES(
    factory_test.cpp
)
```

Standalone checks use the same shape:

```cmake
TESTS(CommonLibs)

TEST(CommonLibs)

SOURCES(
    common_libs.cpp
)

PRIVATE_DEPENDS(
    GLM
    StbImage
    TinyObj
)

TEST_LABELS(
    medium
    cpp
    libs
)
```

Test labels are CTest labels. They are used by runner scripts and custom targets to run batches such as all `small`
tests, all `libs` tests, or all tests owned by a directory-specific label. `TEST_LABELS(...)` also adds automatic labels:

```text
dir:<relative-source-directory>
test:<current-test-name>
```

Production `.cpp` files must not be added to test targets again. Tests use production code through explicit module
dependencies.

## Benchmarks

Create a benchmark suite:

```cmake
BENCHMARKS(Main)

RECURSE(
    smoke
)
```

Add a benchmark group:

```cmake
BENCHMARK(Smoke)

SOURCES(
    smoke.cpp
)

BENCHMARK_LABELS(
    smoke
)
```

`BENCHMARKS(name)` creates the executable. `BENCHMARK(name)` starts a logical benchmark group inside the current suite.
`BENCHMARK()` from a child directory continues the active benchmark group. `BENCHMARK_LABELS(...)` also records automatic
labels:

```text
dir:<relative-source-directory>
benchmark:<current-benchmark-name>
```

Benchmarks are normally filtered at runtime by the benchmark runner, for example with `--benchmark_filter=Smoke`.

## Samples

Create a sample executable:

```cmake
SAMPLE(Base)

SOURCES(
    main.cpp
)

PRIVATE_DEPENDS(
    Window
)
```

Samples intentionally have no `SAMPLE_PART()` API until a sample grows enough structure to need it.

## Sources And Headers

`.cpp` files are always listed explicitly:

```cmake
SOURCES(
    window.cpp
    factory.cpp
)
```

`.h` files in the current directory are collected automatically by entry macros. Header collection is not recursive.

## Dependencies

Implementation-only dependencies:

```cmake
PRIVATE_DEPENDS(
    Vulkan::Vulkan
    Threads::Threads
)
```

Dependencies exposed through the public API:

```cmake
PUBLIC_DEPENDS(
    Common
)
```

Short project names are resolved automatically:

```text
Common -> GraphicsEngine::Common
Window -> GraphicsEngine::Window
```

Qualified CMake targets remain unchanged:

```text
Vulkan::Vulkan
Threads::Threads
GTest::gtest_main
```

The utilities organize common project operations only. Anything outside this abstraction should use regular CMake
directly.

## Internal Layout

`cmake/utils.cmake` is the public include point. The implementation is split by responsibility:

```text
cmake/utils/context.cmake        # current DSL context and local header collection
cmake/utils/target_options.cmake # project-wide target options
cmake/utils/dependencies.cmake   # dependency resolution and public/private links
cmake/utils/sources.cmake        # SOURCES(...)
cmake/utils/modules.cmake        # MODULE(...), SUBMODULE()
cmake/utils/tests.cmake          # TESTS(...), TEST(...), TEST_LABELS(...), TEST_TARGET(...)
cmake/utils/benchmarks.cmake     # BENCHMARKS(...), BENCHMARK(...), BENCHMARK_LABELS(...)
cmake/utils/samples.cmake        # SAMPLE(...)
cmake/utils/recurse.cmake        # RECURSE(...) and entry-file discovery
cmake/utils/external.cmake       # include_external(...)
cmake/utils/package_api.cmake    # PACKAGE_ROOT(...), PACKAGE_COMPONENT(...), PACKAGE_DEPENDS(...)
```

Files under `cmake/utils/` are implementation details of the project DSL. Module entry files should include
`cmake/utils.cmake` through the top-level project setup, not the internal files directly.

## Package

GraphicsEngine can be installed as a CMake package.

The public connection point is:

```cmake
GraphicsEngine::GraphicsEngine
```

Internal module targets are not part of the public CMake API.

Normal usage:

```cmake
find_package(
    GraphicsEngine REQUIRED CONFIG
)

target_link_libraries(
    MyApplication
    PRIVATE
    GraphicsEngine::GraphicsEngine
)
```

If an application needs a specific installed capability:

```cmake
find_package(
    GraphicsEngine REQUIRED CONFIG
    COMPONENTS Vulkan GLFW
)

target_link_libraries(
    MyApplication
    PRIVATE
    GraphicsEngine::GraphicsEngine
)
```

Components check installed build capabilities and do not add separate public targets.
