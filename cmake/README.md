# CMake Utilities

`utils.cmake` provides a small DSL for declaring GraphicsEngine modules, sources, tests, and dependencies.

## Entry files

Directories participating in the build use one of four entry files:

```text id="6kk6k6"
module.cmake
submodule.cmake
test_module.cmake
test_submodule.cmake
```

`RECURSE()` automatically finds the appropriate entry file. A directory must contain exactly one of them.

## Production modules

Create a module:

```cmake id="6i9ktx"
MODULE(Core)

SOURCES(
    engine.cpp
)

RECURSE(
    resource
    test
)
```

This creates:

```text id="b8dx3b"
GraphicsEngineCore
GraphicsEngine::Core
```

Extend an existing module from a child directory:

```cmake id="yox9wn"
SUBMODULE()

SOURCES(
    resource.cpp
)
```

`SUBMODULE()` does not create another target.

## Tests

`MODULE(Core)` also creates the test target when tests are enabled:

```text id="lndgo7"
GraphicsEngineCoreTests
GraphicsEngine::CoreTests
```

It automatically links against `GraphicsEngine::Core`.

Root test directory:

```cmake id="4p3hkh"
TEST_MODULE(Core)

SOURCES(
    engine.cpp
)

PRIVATE_DEPENDS(
    GTest::gtest_main
)
```

Nested test directory:

```cmake id="gzxwhf"
TEST_SUBMODULE(Core)

SOURCES(
    resource.cpp
)
```

Production `.cpp` files must not be added to test targets again. Tests use production code through the corresponding
module dependency.

## Sources and headers

`.cpp` files are always listed explicitly:

```cmake id="4eknma"
SOURCES(
    resource.cpp
    resource_manager.cpp
)
```

`.h` files in the current directory are collected automatically by:

```text id="v5opel"
MODULE
SUBMODULE
TEST_MODULE
TEST_SUBMODULE
```

Header collection is not recursive.

## Dependencies

Implementation-only dependencies:

```cmake id="6ed8b8"
PRIVATE_DEPENDS(
    Vulkan::Vulkan
    Threads::Threads
)
```

Dependencies exposed through the public API:

```cmake id="gy16dz"
PUBLIC_DEPENDS(
    Core
)
```

Short project names are resolved automatically:

```text id="7y8ur3"
Core -> GraphicsEngine::Core
Vulkan -> GraphicsEngine::Vulkan
```

Qualified CMake targets remain unchanged:

```text id="97rxlj"
Vulkan::Vulkan
Threads::Threads
GTest::gtest_main
```

## Example

```text id="odjwdv"
graphics/
├── module.cmake
├── graphics.h
├── graphics.cpp
├── resource/
│   ├── submodule.cmake
│   ├── resource.h
│   ├── resource.cpp
│   └── test/
│       ├── test_submodule.cmake
│       └── resource.cpp
└── test/
    ├── test_module.cmake
    └── graphics.cpp
```

```cmake id="ulmhni"
# graphics/module.cmake

MODULE(Core)

SOURCES(
    graphics.cpp
)

RECURSE(
    resource
    test
)
```

```cmake id="86u6g3"
# graphics/resource/submodule.cmake

SUBMODULE()

SOURCES(
    resource.cpp
)

RECURSE(
    test
)
```

```cmake id="3clwsf"
# graphics/resource/test/test_submodule.cmake

TEST_SUBMODULE(Core)

SOURCES(
    resource.cpp
)
```

The utilities organize common module operations only. Anything outside this abstraction should use regular CMake
directly.