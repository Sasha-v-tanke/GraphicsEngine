# GraphicsEngine Style Guide

This document defines the project-wide code style for GraphicsEngine.

The formatter baseline is **clang-format 23.1.0**. Formatting rules enforced by
clang-format are duplicated here intentionally so that the style remains readable
without inspecting the formatter configuration.

## 1. C++ language

- Use C++23.
- Use 4 spaces for indentation. Tabs are not used.
- Maximum line width is 120 columns.
- Opening braces stay on the same line.
- Do not put non-empty functions, control-flow blocks, or class bodies on one line.
- Use one empty line at most between code blocks.
- Files end with a newline.
- Pointer and reference symbols bind to the type:

```cpp
Device* device = nullptr;
const Resource& resource = GetResource();
```

## 2. Naming

### 2.1. Types

Classes, structs, aliases, concepts, and template type parameters use `PascalCase`.

```cpp
class ResourceManager;
struct TextureDescription;
using ResourceHandle = uint64_t;

template<typename ResourceType>
class ResourcePool;
```

### 2.2. Interfaces

Polymorphic interfaces use the `I<Name>` form.

```cpp
class IGraphicsBackend;
class IResourceProvider;
```

Use the `I` prefix only for actual interfaces.

### 2.3. Implementation types

An implementation class may use `<Name>Impl` when it implements a corresponding
abstraction.

```cpp
class EngineImpl;
class ResourceManagerImpl;
```

Do not add `Impl` merely because a class is defined in a `.cpp` file.

### 2.4. Namespaces

Namespaces use the `N<Name>` form.

```cpp
namespace NGraphics {

class Device;

} // namespace NGraphics
```

Nested namespaces use the same rule:

```cpp
namespace NGraphics::NVulkan {

class Device;

} // namespace NGraphics::NVulkan
```

Examples:

- `NEngine`
- `NGraphics`
- `NGraphics::NVulkan`
- `NGraphics::NOpenGL`
- `NCommon`

Avoid `using namespace`. It is forbidden in headers and should not be used in
engine `.cpp` files unless there is an exceptional local reason. Specific `using`
declarations are allowed.

### 2.5. Functions and methods

Functions and methods use `PascalCase`.

```cpp
void DrawFrame();
ResourceManager& GetResourceManager();
bool IsComplete() const;
```

### 2.6. Local variables and parameters

Local variables and function parameters use `camelCase`.

```cpp
uint32_t frameIndex = 0;
void SetActiveScene(SceneHandle scene);
```

### 2.7. Private data members

Private data members use the `m_<camelCase>` form.

```cpp
Device m_device;
uint32_t m_currentFrame = 0;
```

### 2.8. Public struct fields

Public data fields use `PascalCase`.

```cpp
struct TextureDescription {
    uint32_t Width = 0;
    uint32_t Height = 0;
    ETextureFormat Format = ETextureFormat::RGBA8;
};
```

### 2.9. Enums

Enum types use the `E<Name>` form.

Enum values use `UPPER_SNAKE_CASE`.

Each enum value is always written on a separate line.

```cpp
enum class EGraphicsBackend {
    VULKAN,
    OPENGL,
    METAL,
};
```

### 2.10. Constants

Namespace-scope and class-scope constants use `UPPER_SNAKE_CASE`.

```cpp
constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;
```

Function-local constants follow normal local-variable naming and use `camelCase`.

```cpp
constexpr uint32_t frameCount = 2;
```

### 2.11. Macros

Macros use `UPPER_SNAKE_CASE`.

Public/project-wide macros should use a project prefix.

```cpp
GRAPHICS_ENGINE_ASSERT(...);
GRAPHICS_ENGINE_ENSURE(...);
```

Backend-specific macros may include the backend in the prefix.

```cpp
GRAPHICS_ENGINE_VULKAN_CHECK(...);
```

### 2.12. Acronyms

Acronyms are treated as normal words in identifiers.

Use:

```text
GpuResource
CpuBuffer
ApiVersion
Url
Uuid
```

Do not use:

```text
GPUResource
CPUBuffer
APIVersion
URL
UUID
```

Official technology names keep their established spelling where appropriate,
for example `OpenGL` and `Vulkan`.

## 3. Type spelling and declarations

### 3.1. `auto`

Prefer explicit types.

Use `auto` only when the explicit type is genuinely inconvenient, excessively
verbose, implementation-dependent, or for iterator types.

Prefer:

```cpp
VkResult result = vkCreateDevice(...);
CompletionToken completion = Submit(...);

for (std::vector<Resource>::iterator it = resources.begin(); it != resources.end(); ++it) {
    // `auto` is allowed here when the iterator type is inconvenient.
}
```

Typical acceptable uses include:

```cpp
auto it = resources.find(handle);
```

Do not use `auto` merely to avoid writing a short, useful type.

### 3.2. Type aliases

Use `using`, not `typedef`.

```cpp
using ResourceId = uint64_t;
```

Do not create aliases merely to hide raw-pointer syntax.

Avoid:

```cpp
using DevicePtr = Device*;
```

Prefer:

```cpp
Device* device = nullptr;
```

Semantic handle aliases are allowed when the alias represents a real concept.

### 3.3. Smart pointers

Spell ownership explicitly.

```cpp
std::unique_ptr<Device>
std::shared_ptr<Resource>
std::weak_ptr<Resource>
```

Do not introduce aliases such as `DeviceUniquePtr` without a concrete semantic
reason.

### 3.4. Integer types

Use fixed-width integer types where width is part of the contract, binary
layout, serialization format, graphics API, or public API.

```cpp
uint32_t
uint64_t
int32_t
```

Use `size_t` where it naturally represents a container or memory size.

### 3.5. Casts

Do not use C-style casts.

Use the appropriate C++ cast:

```cpp
static_cast<Type>(value);
reinterpret_cast<Type*>(pointer);
const_cast<Type*>(pointer);
```

`reinterpret_cast` and `const_cast` should remain uncommon and require a clear
reason.

### 3.6. Null pointers

Use `nullptr`.

API-specific null handles such as `VK_NULL_HANDLE` remain valid.

## 4. Classes and structs

### 4.1. `struct` vs `class`

Use `struct` for simple value/data types with public state.

Use `class` for types with invariants, encapsulation, ownership, or meaningful
behavior.

### 4.2. Class member ordering

Prefer the following order:

1. public nested types and constants;
2. constructors and destructor;
3. public methods;
4. protected section, only if needed;
5. private helper methods;
6. private data members.

Private helper methods and private data members use separate `private:` sections.

```cpp
class Device: public NCommon::NonCopyable {
public:
    explicit Device(const DeviceDescription& description);

    ~Device();

    void Initialize();
    void Cleanup();

    VkDevice GetHandle() const noexcept;

private:
    void CreateLogicalDevice();
    void SelectQueues();

private:
    VkDevice m_device = VK_NULL_HANDLE;
};
```

### 4.3. Ownership and transfer restrictions

Do not repeatedly spell copy/move restrictions with groups of deleted special
members when the project has a corresponding reusable ownership base.

Use project utility bases such as:

```cpp
NCommon::NonCopyable
NCommon::NonTransferable
```

The exact semantics of these base classes must remain clearly defined in their
own API.

### 4.4. Constructors

Single-argument constructors are `explicit` unless implicit conversion is an
intentional part of the type's semantics.

```cpp
explicit EngineImpl(const EngineConfig& config);
```

Constructor initializer lists use leading commas:

```cpp
EngineImpl::EngineImpl(const EngineConfig& config)
    : m_graphicsRuntime(config)
    , m_activeScene() {
}
```

### 4.5. Overrides

Use `override` on overrides.

Do not repeat `virtual` on an overriding declaration.

```cpp
void DrawFrame() override;
```

Use `final` when further inheritance is intentionally forbidden.

### 4.6. `noexcept`

Use `noexcept` when it is part of the real semantic contract.

Typical uses include trivial accessors and operations that genuinely guarantee
not to throw.

Do not add `noexcept` mechanically.

### 4.7. Public fields

Mutable public fields are appropriate for simple data/description/configuration
structs.

Classes should normally keep mutable state private.

## 5. Functions

### 5.1. `const`

Use `const` references when mutation is not required.

```cpp
void Submit(const FrameSubmission& submission);
```

Do not add top-level `const` to scalar/by-value parameters merely for style.

Prefer:

```cpp
void Wait(CompletionToken completion);
```

not:

```cpp
void Wait(const CompletionToken completion);
```

Local variables that are not modified after initialization should normally be
`const`.

```cpp
const VkResult result = vkCreateDevice(...);
```

### 5.2. Boolean naming

Boolean variables use descriptive prefixes such as:

```text
isValid
isInitialized
hasSurface
canPresent
shouldRecreate
```

Boolean methods use:

```text
IsValid()
IsInitialized()
HasSurface()
CanPresent()
ShouldRecreate()
```

Avoid names such as `GetIsValid()`.

### 5.3. Getters and setters

Property-like accessors use `Get<Name>` / `Set<Name>` where appropriate.

```cpp
uint32_t GetWidth() const noexcept;
void SetActiveScene(SceneHandle scene);
```

Operations should use verbs that describe the operation rather than being
forced into `Get...`.

```cpp
DeviceCapabilities QueryCapabilities();
```

### 5.4. Early returns

Prefer early returns when they reduce nesting.

```cpp
if (!IsValid()) {
    return;
}

DoWork();
```

### 5.5. Range-based loops

Do not put a space before the range colon.

Prefer an explicit element type whenever practical.

```cpp
for (const Resource& resource: resources) {
    Process(resource);
}
```

Use `auto` only when the element type is genuinely inconvenient.

### 5.6. Lambdas

Keep capture lists minimal.

Prefer explicit captures for long-lived or asynchronous lambdas.

```cpp
[this, frameIndex]
```

A local `[&]` capture is acceptable when its scope and lifetime are obvious.

### 5.7. Templates

Template declarations stay on their own line.

```cpp
template<typename T>
void Process(T&& value);
```

Use `typename` consistently for template type parameters.

Concept names use `PascalCase`.

```cpp
template<typename T>
concept GraphicsResource = ...;
```

## 6. Control flow

### 6.1. `switch`

`case` labels are aligned with `switch`.

Do not insert blank lines between cases.

Do not use implicit or explicit fallthrough between cases. Each case must
terminate through `break`, `return`, `throw`, or equivalent control transfer.

```cpp
switch (backend) {
case EGraphicsBackend::VULKAN:
    return CreateVulkanBackend();
case EGraphicsBackend::OPENGL:
    return CreateOpenGLBackend();
}
```

For exhaustive enum switches, avoid an unnecessary `default` so compiler
diagnostics can detect newly added enum values.

### 6.2. Comparisons

Write comparisons naturally.

```cpp
pointer == nullptr
count == 0
```

Do not use Yoda-style comparisons.

## 7. Initialization

Prefer value initialization when an object should be zero/default initialized.

```cpp
VkDeviceCreateInfo createInfo{};
```

Use designated initializers for aggregates when they improve clarity.

```cpp
TextureDescription description{
    .Width = width,
    .Height = height,
    .Format = ETextureFormat::RGBA8,
};
```

Use a trailing comma in multiline initializer lists, enum lists, and similar
multiline lists where the language permits it.

Declare one variable per declaration.

Prefer:

```cpp
uint32_t width = 0;
uint32_t height = 0;
```

not:

```cpp
uint32_t width = 0, height = 0;
```

## 8. Headers and includes

### 8.1. Header protection

Use:

```cpp
#pragma once
```

Do not use manual include guards for project headers.

### 8.2. Include what you use

Every header must directly include the headers required for the declarations it
contains. Do not rely on transitive includes.

### 8.3. Include order

In a `.cpp` file, its corresponding header comes first.

Then group includes as:

1. C and C++ standard library;
2. third-party/external libraries;
3. project headers.

Separate groups with one empty line.

Example:

```cpp
#include "device.h"

#include <algorithm>
#include <vector>

#include <vulkan/vulkan.h>

#include <graphics/resources/resource.h>
#include <lib/common/exception.h>
```

Sort includes alphabetically inside a group.

Use quotes for the corresponding local header:

```cpp
#include "device.h"
```

Use angle brackets for other project, standard-library, and external headers.

### 8.4. Forward declarations

Use forward declarations when they meaningfully reduce dependency coupling and a
complete type is not required.

Do not manually forward-declare standard-library types.

## 9. Source organization

### 9.1. Header/source pairs

Project C++ files use:

```text
snake_case.h
snake_case.cpp
```

Do not mix `.hpp`, `.hh`, `.cc`, or `.cxx` for project code without an explicit
exception.

### 9.2. Directories

Directories use `snake_case`.

```text
graphics/
graphics/backend/
graphics/backend/vulkan/
graphics/resources/
```

### 9.3. File names and directory context

Name a file after its responsibility or primary entity, but do not repeat context
that is already unambiguous from the directory path.

Prefer:

```text
graphics/backend/vulkan/device.h
graphics/backend/vulkan/command_buffer.h
graphics/backend/vulkan/pipeline.h
```

over:

```text
graphics/backend/vulkan/vulkan_device.h
graphics/backend/vulkan/vulkan_command_buffer.h
graphics/backend/vulkan/vulkan_pipeline.h
```

The C++ type may still include backend context where useful, for example
`VulkanDevice`.

### 9.4. Local utility files

Names such as the following are valid:

```text
helper.cpp
utils.cpp
common.cpp
```

They are appropriate when the file contains cohesive local helpers or shared
implementation details for the surrounding module.

Do not use such files as unrelated catch-all storage.

### 9.5. File-local implementation helpers

Use an anonymous namespace for free functions and objects that are private to a
`.cpp` file.

```cpp
namespace {

bool IsSuitableDevice(...) {
    ...
}

} // namespace
```

### 9.6. Tests

Tests live in `test/` directories.

A repository may contain multiple `test/` directories at different module
levels.

Test file names describe the tested entity or functionality and do not contain a
`test` suffix merely because they are test files.

Examples:

```text
graphics/resource/test/resource_manager.cpp
graphics/backend/vulkan/test/command_buffer.cpp
math/test/matrix.cpp
```

## 10. Comments

Comments should primarily explain:

- why a decision exists;
- non-obvious invariants;
- ownership and lifetime requirements;
- synchronization requirements;
- platform/backend quirks;
- non-obvious performance choices.

Do not comment obvious code.

Avoid:

```cpp
// Increment frame index.
++frameIndex;
```

Prefer comments that explain the reason behind unusual behavior.

### 10.1. TODO

New TODO comments are initially written without a task tag.

```cpp
// TODO: Support multiple graphics queues.
```

A TODO that represents real tracked work should be surfaced during review. The
review process should ask for a task to be created and then update the TODO with
the resulting task identifier/tag.

Do not invent task identifiers in advance.

### 10.2. Public API documentation

Document public API behavior when the signature alone does not make important
semantics clear, especially:

- ownership;
- lifetime;
- thread safety;
- synchronization;
- valid states;
- failure behavior.

Do not add documentation comments to trivial accessors solely for completeness.

## 11. CMake style

This section defines syntax and naming style only. The source-listing/module
architecture is intentionally not fixed yet.

### 11.1. Commands

CMake commands use lowercase and have no space before `(`.

```cmake
add_library(...)
target_link_libraries(...)
if (...)
endif ()
```

### 11.2. Variables and options

Project variables and options use `UPPER_SNAKE_CASE`.

Project-wide options use the project prefix.

```cmake
GRAPHICS_ENGINE_BUILD_SAMPLES
GRAPHICS_ENGINE_BUILD_TESTS
GRAPHICS_ENGINE_ENABLE_VULKAN
```

Avoid generic options such as `BUILD_SAMPLES` in library projects.

### 11.3. Targets

Internal physical target names use project-specific `PascalCase` names.

Public aliases use the `GraphicsEngine::<Name>` namespace.

```cmake
add_library(GraphicsEngineCore STATIC ...)
add_library(GraphicsEngine::Core ALIAS GraphicsEngineCore)

add_library(GraphicsEngineVulkan STATIC ...)
add_library(GraphicsEngine::Vulkan ALIAS GraphicsEngineVulkan)
```

### 11.4. Multiline target commands

Scopes such as `PUBLIC`, `PRIVATE`, and `INTERFACE` are indented one level.
Their values are indented one additional level.

```cmake
target_link_libraries(Target
    PUBLIC
    Foo
    Bar
    PRIVATE
    Baz
)
```

Use the same layout for similar scoped commands such as
`target_include_directories`.

### 11.5. Conditions

Use:

```cmake
if (GRAPHICS_ENGINE_BUILD_SAMPLES)
    add_subdirectory(samples)
endif ()
```

Do not use:

```cmake
if (GRAPHICS_ENGINE_BUILD_SAMPLES)
endif ()
```

### 11.6. Source listing

The source-listing and recursive module mechanism is intentionally not specified
by this style guide yet. It will be designed separately.

## 12. Formatting and linting tooling

The project standard formatter version is:

```text
clang-format 23.1.0
```

Repository formatting tooling should live under:

```text
tools/
    formatter/
        run.sh
        check.sh
        ignore
        ...
```

Formatting and linting are separate responsibilities.

Formatter tooling changes formatting only.

Lint/static-analysis checks are treated as tests and should live in the testing
or checking infrastructure rather than being folded into the formatter.

External, generated, vendored, and build output should not be modified by
project formatters.

Typical excluded areas include:

```text
external/
third_party/
build/
cmake-build-*/
generated/
```

The exact ignore syntax and formatter orchestration are defined by the formatter
tooling itself.
