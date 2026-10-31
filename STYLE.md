# Правила оформления кода GraphicsEngine

Этот документ устанавливает единые правила оформления исходного кода GraphicsEngine. Основная версия средства форматирования — **clang-format 23.1.0**. Некоторые автоматически проверяемые правила продублированы здесь для самостоятельного чтения документа.

## 1. Язык C++

- Использовать C++23.
- Отступ — четыре пробела, табуляция не применяется.
- Максимальная длина строки — 120 столбцов.
- Открывающая фигурная скобка располагается на строке объявления.
- Непустые функции, тела классов и блоки управления не помещаются целиком в одну строку.
- Между блоками кода не более одной пустой строки; файл заканчивается переводом строки.
- Указатель и ссылка пишутся рядом с типом:

```cpp
Device* device = nullptr;
const Resource& resource = GetResource();
```

## 2. Именование

| Сущность | Правило | Пример |
| --- | --- | --- |
| Классы, структуры, псевдонимы, концепты, параметры типов | `PascalCase` | `ResourceManager` |
| Полиморфные интерфейсы | `I<Name>` | `IGraphicsBackend` |
| Классы реализации соответствующей абстракции | `<Name>Impl` | `EngineImpl` |
| Пространства имён | `N<Name>`, включая вложенные | `NGraphics::NVulkan` |
| Функции и методы | `PascalCase` | `DrawFrame()` |
| Локальные переменные и параметры | `camelCase` | `frameIndex` |
| Закрытые поля | `m_<camelCase>` | `m_currentFrame` |
| Открытые поля структур | `PascalCase` | `Width` |
| Типы перечислений | `E<Name>` | `EGraphicsBackend` |
| Элементы перечислений и макросы | `UPPER_SNAKE_CASE` | `VULKAN` |
| Константы в пространстве имён и классе | `UPPER_SNAKE_CASE` | `MAX_FRAMES_IN_FLIGHT` |
| Константы внутри функций | `camelCase` | `frameCount` |

Префикс `I` допускается только для настоящего интерфейса. Суффикс `Impl` не добавляется только потому, что класс определён в `.cpp`. Макросы, общие для проекта, должны иметь префикс `GRAPHICS_ENGINE_`; допустим префикс реализации, например `GRAPHICS_ENGINE_VULKAN_CHECK`. Каждое значение перечисления пишется на отдельной строке.

Аббревиатуры в идентификаторах считаются обычными словами: `GpuResource`, `CpuBuffer`, `ApiVersion`, `Url`, `Uuid`; не `GPUResource`, `CPUBuffer`, `APIVersion`, `URL`, `UUID`. Имена технологий сохраняют принятую форму, например `OpenGL` и `Vulkan`.

Избегать `using namespace`: в заголовках он запрещён, в файлах реализации допускается лишь при исключительном локальном обосновании. Отдельные объявления `using` разрешены.

```cpp
namespace NGraphics::NVulkan {

class Device;

} // namespace NGraphics::NVulkan

struct TextureDescription {
    uint32_t Width = 0;
    uint32_t Height = 0;
    ETextureFormat Format = ETextureFormat::RGBA8;
};
```

### 2.1. Типы

Пример:

```cpp
class ResourceManager;
struct TextureDescription;
using ResourceHandle = uint64_t;

template<typename ResourceType>
class ResourcePool;
```

### 2.2. Интерфейсы

Пример:

```cpp
class IGraphicsBackend;
class IResourceProvider;
```

### 2.3. Типы реализаций

Пример:

```cpp
class EngineImpl;
class ResourceManagerImpl;
```

### 2.4. Пространства имён

Пример:

```cpp
namespace NGraphics {

class Device;

} // namespace NGraphics
```

Пример:

```cpp
namespace NGraphics::NVulkan {

class Device;

} // namespace NGraphics::NVulkan
```

### 2.5. Функции и методы

Пример:

```cpp
void DrawFrame();
ResourceManager& GetResourceManager();
bool IsComplete() const;
```

### 2.6. Локальные переменные и параметры

Пример:

```cpp
uint32_t frameIndex = 0;
void SetActiveScene(SceneHandle scene);
```

### 2.7. Закрытые поля

Пример:

```cpp
Device m_device;
uint32_t m_currentFrame = 0;
```

### 2.8. Открытые поля структур

Пример:

```cpp
struct TextureDescription {
    uint32_t Width = 0;
    uint32_t Height = 0;
    ETextureFormat Format = ETextureFormat::RGBA8;
};
```

### 2.9. Перечисления

Пример:

```cpp
enum class EGraphicsBackend {
    VULKAN,
    OPENGL,
    METAL,
};
```

### 2.10. Константы

Пример:

```cpp
constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;
```

Пример:

```cpp
constexpr uint32_t frameCount = 2;
```

### 2.11. Макросы

Пример:

```cpp
GRAPHICS_ENGINE_ASSERT(...);
GRAPHICS_ENGINE_ENSURE(...);
```

Пример:

```cpp
GRAPHICS_ENGINE_VULKAN_CHECK(...);
```

### 2.12. Аббревиатуры

Предпочтительные имена:

```text
GpuResource
CpuBuffer
ApiVersion
Url
Uuid
```

Неправильные варианты:

```text
GPUResource
CPUBuffer
APIVersion
URL
UUID
```

## 3. Типы и объявления

Предпочтительны явные типы. `auto` используется, когда тип неудобен, избыточно длинен, зависит от реализации или представляет итератор. Не скрывать короткий полезный тип только ради краткости.

Использовать `using` вместо `typedef`. Не создавать псевдонимы лишь для маскировки указателя вроде `DevicePtr = Device*`; смысловые псевдонимы дескрипторов допустимы. Владение выражать стандартными `std::unique_ptr`, `std::shared_ptr` и `std::weak_ptr` без необоснованных дополнительных имён.

Типы целых чисел фиксированной ширины использовать, когда ширина является частью контракта, двоичного представления, сериализации, графического или публичного интерфейса. `size_t` применять для размеров контейнеров и памяти. Преобразования в стиле C запрещены; применять подходящий `static_cast`, `reinterpret_cast` или `const_cast`, причём последние два требуют ясного обоснования. Нулевые указатели записывать через `nullptr`; специальные значения интерфейса, например `VK_NULL_HANDLE`, допускаются.

### 3.1. Выбор `auto`

Предпочтительны явные типы:

```cpp
VkResult result = vkCreateDevice(...);
CompletionToken completion = Submit(...);

for (std::vector<Resource>::iterator it = resources.begin(); it != resources.end(); ++it) {
    // `auto` is allowed here when the iterator type is inconvenient.
}
```

Допустимо при неудобном типе итератора:

```cpp
auto it = resources.find(handle);
```

### 3.2. Псевдонимы типов

Смысловой псевдоним:

```cpp
using ResourceId = uint64_t;
```

Не рекомендуется использовать псевдоним только для указателя:

```cpp
using DevicePtr = Device*;
```

Предпочтительно:

```cpp
Device* device = nullptr;
```

### 3.3. Умные указатели

Пример:

```cpp
std::unique_ptr<Device>
std::shared_ptr<Resource>
std::weak_ptr<Resource>
```

### 3.4. Целые числа

Пример:

```cpp
uint32_t
uint64_t
int32_t
```

### 3.5. Преобразования типов

Пример:

```cpp
static_cast<Type>(value);
reinterpret_cast<Type*>(pointer);
const_cast<Type*>(pointer);
```

## 4. Классы и структуры

`struct` предназначен для простых типов данных с открытым состоянием, `class` — для сущностей с инвариантами, инкапсуляцией, владением или поведением.

Предпочтительный порядок: открытые вложенные типы и константы; конструкторы и деструктор; открытые методы; при необходимости защищённый раздел; закрытые вспомогательные методы; закрытые поля. Для последних двух использовать отдельные разделы `private:`.

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

При существующей общей основе владения не дублировать группы удалённых специальных методов. Использовать соответствующие `NCommon::NonCopyable` или `NCommon::NonTransferable`, не меняя их собственных контрактов.

Конструктор с одним параметром — `explicit`, кроме намеренно допустимого неявного преобразования. В списках инициализации конструкторов запятые находятся в начале строк:

```cpp
EngineImpl::EngineImpl(const EngineConfig& config)
    : m_graphicsRuntime(config)
    , m_activeScene() {
}
```

Для переопределений использовать `override`, не повторяя `virtual`. `final` применять при намеренном запрете наследования. `noexcept` обозначает реальную гарантию отсутствия исключений и не добавляется механически. Изменяемые открытые поля допустимы для простых структур данных или конфигураций, но состояние классов обычно закрыто.

### 4.3. Ограничения владения и копирования

Пример:

```cpp
NCommon::NonCopyable
NCommon::NonTransferable
```

### 4.4. Конструкторы

Пример:

```cpp
explicit EngineImpl(const EngineConfig& config);
```

### 4.5. Переопределения

Пример:

```cpp
void DrawFrame() override;
```

## 5. Функции

Если изменение аргумента не требуется, использовать ссылку на `const`. Не добавлять верхнеуровневый `const` к скалярному параметру по значению только ради оформления. Локальные переменные, не изменяемые после инициализации, обычно объявлять `const`.

Логические переменные использовать с осмысленными префиксами `isValid`, `hasSurface`, `canPresent`, `shouldRecreate`. Методы — `IsValid()`, `HasSurface()`, `CanPresent()`; избегать `GetIsValid()`. Доступ к свойствам оформлять через `Get<Name>` и `Set<Name>`, а самостоятельные операции называть глаголом, например `QueryCapabilities()`.

Предпочитать ранний возврат, если он уменьшает вложенность. В цикле по диапазону не ставить пробел перед двоеточием и по возможности писать явный тип элемента:

```cpp
for (const Resource& resource: resources) {
    Process(resource);
}
```

Списки захватов лямбд должны быть минимальными; в долгоживущих и асинхронных лямбдах предпочитать явный захват. Локальный `[&]` допустим при очевидной области видимости и времени жизни.

Объявления шаблонов располагать на отдельной строке. Для параметров типов последовательно использовать `typename`, а имена концептов писать в `PascalCase`.

```cpp
template<typename T>
concept GraphicsResource = ...;
```

### 5.1. Постоянство значений и `const`

Параметр по постоянной ссылке:

```cpp
void Submit(const FrameSubmission& submission);
```

Предпочтительно передавать значение без избыточного верхнеуровневого `const`:

```cpp
void Wait(CompletionToken completion);
```

Не рекомендуется:

```cpp
void Wait(const CompletionToken completion);
```

Локальная неизменяемая переменная:

```cpp
const VkResult result = vkCreateDevice(...);
```

### 5.2. Именование логических значений

Логические переменные:

```text
isValid
isInitialized
hasSurface
canPresent
shouldRecreate
```

Методы, возвращающие логические значения:

```text
IsValid()
IsInitialized()
HasSurface()
CanPresent()
ShouldRecreate()
```

### 5.3. Методы доступа к свойствам

Методы доступа:

```cpp
uint32_t GetWidth() const noexcept;
void SetActiveScene(SceneHandle scene);
```

Самостоятельная операция:

```cpp
DeviceCapabilities QueryCapabilities();
```

### 5.4. Ранние возвраты

Пример:

```cpp
if (!IsValid()) {
    return;
}

DoWork();
```

### 5.6. Захваты лямбда-выражений

Пример:

```cpp
[this, frameIndex]
```

### 5.7. Шаблоны и концепты

Пример:

```cpp
template<typename T>
void Process(T&& value);
```

## 6. Управляющие конструкции

Метки `case` выравниваются с `switch`, без пустых строк между ветвями. Не допускать неявного или явного перехода из одной ветви в другую: каждая заканчивается `break`, `return`, `throw` или эквивалентом. Для полного `switch` по перечислению не добавлять необязательный `default`, чтобы компилятор выявлял новые значения.

Сравнения записывать естественно: `pointer == nullptr`, `count == 0`. Не менять местами константу и значение ради особого стиля.

### 6.1. Ветвление `switch`

Пример:

```cpp
switch (backend) {
case EGraphicsBackend::VULKAN:
    return CreateVulkanBackend();
case EGraphicsBackend::OPENGL:
    return CreateOpenGLBackend();
}
```

### 6.2. Сравнения

Пример:

```cpp
pointer == nullptr
count == 0
```

## 7. Инициализация

Предпочитать инициализацию значением для нулевого или стандартного состояния:

```cpp
VkDeviceCreateInfo createInfo{};

TextureDescription description{
    .Width = width,
    .Height = height,
    .Format = ETextureFormat::RGBA8,
};
```

Использовать именованные инициализаторы агрегатов, когда они повышают понятность. В многострочных списках инициализации и перечислениях ставить завершающую запятую там, где язык её допускает. Одно объявление — одна переменная.

### Дополнительные примеры и противопоставления

Инициализация значением:

```cpp
VkDeviceCreateInfo createInfo{};
```

Именованная инициализация агрегата:

```cpp
TextureDescription description{
    .Width = width,
    .Height = height,
    .Format = ETextureFormat::RGBA8,
};
```

Предпочтительно объявлять переменные отдельно:

```cpp
uint32_t width = 0;
uint32_t height = 0;
```

Не рекомендуется объединять объявления:

```cpp
uint32_t width = 0, height = 0;
```

## 8. Заголовочные файлы

Применять `#pragma once` вместо ручной защиты от повторного включения. Каждый заголовок обязан самостоятельно подключать необходимые зависимости; нельзя полагаться на транзитивные включения.

В `.cpp` первым подключать соответствующий заголовок, затем группы стандартной библиотеки, сторонних библиотек и заголовков проекта. Группы разделять пустой строкой и сортировать внутри по алфавиту. Для собственного заголовка использовать кавычки, для остальных — угловые скобки.

```cpp
#include "device.h"

#include <algorithm>
#include <vector>

#include <vulkan/vulkan.h>

#include <graphics/resources/resource.h>
#include <lib/common/exception.h>
```

Предварительные объявления применять для снижения связанности, когда полный тип не нужен. Не объявлять типы стандартной библиотеки вручную.

### 8.1. Защита заголовка

Пример:

```cpp
#pragma once
```

### 8.3. Порядок включений

Пример:

```cpp
#include "device.h"
```

## 9. Организация исходников

Имена исходников: `snake_case.h` и `snake_case.cpp`, каталогов — `snake_case`. Другие расширения (`.hpp`, `.hh`, `.cc`, `.cxx`) требуют явного исключения.

Имя файла должно отражать ответственность, не дублируя однозначный контекст каталога: `graphics/backend/vulkan/device.h` предпочтительнее `graphics/backend/vulkan/vulkan_device.h`. Имя класса при этом может содержать `Vulkan`. `helper.cpp`, `utils.cpp` и `common.cpp` допустимы для связанных локальных вспомогательных функций, но не как свалка несвязанных реализаций.

Свободные функции и объекты, принадлежащие только одному `.cpp`, размещать в анонимном пространстве имён.

Тесты располагаются в каталогах `test/`, в том числе на разных уровнях модулей. Файл теста называют по проверяемому объекту или поведению без суффикса `test`, добавленного только из-за его назначения: `math/test/matrix.cpp`, `graphics/backend/vulkan/test/command_buffer.cpp`.

### 9.1. Пары заголовков и исходников

Пример:

```text
snake_case.h
snake_case.cpp
```

### 9.2. Названия каталогов

Пример:

```text
graphics/
graphics/backend/
graphics/backend/vulkan/
graphics/resources/
```

### 9.3. Имена файлов и контекст каталогов

Предпочтительные имена файлов:

```text
graphics/backend/vulkan/device.h
graphics/backend/vulkan/command_buffer.h
graphics/backend/vulkan/pipeline.h
```

Избыточное повторение контекста каталога:

```text
graphics/backend/vulkan/vulkan_device.h
graphics/backend/vulkan/vulkan_command_buffer.h
graphics/backend/vulkan/vulkan_pipeline.h
```

### 9.4. Локальные вспомогательные файлы

Пример:

```text
helper.cpp
utils.cpp
common.cpp
```

### 9.5. Вспомогательные функции в файле реализации

Пример:

```cpp
namespace {

bool IsSuitableDevice(...) {
    ...
}

} // namespace
```

### 9.6. Файлы тестов

Пример:

```text
graphics/resource/test/resource_manager.cpp
graphics/backend/vulkan/test/command_buffer.cpp
math/test/matrix.cpp
```

## 10. Комментарии

Комментарии прежде всего объясняют причины решений, неочевидные инварианты, владение, время жизни, синхронизацию, особенности платформы/реализации и нетривиальные решения по производительности. Не комментировать очевидные операции.

Новые замечания `TODO` сначала записываются без номера задачи. При проверке работы для действительно отслеживаемого замечания создаётся задача, после чего добавляется её настоящий идентификатор. Нельзя придумывать номера заранее.

Публичный интерфейс документируется, если сигнатура не раскрывает важные правила: владение, время жизни, потокобезопасность, синхронизацию, допустимые состояния и ошибки. Очевидные методы доступа не требуют комментариев ради формальной полноты.

### Пример избыточного комментария

Неудачный комментарий, пересказывающий очевидное действие:

```cpp
// Increment frame index.
++frameIndex;
```

### Пример заметки TODO

Новая заметка в исходном коде не должна содержать выдуманного идентификатора задачи:

```cpp
// TODO: Support multiple graphics queues.
```

## 11. Стиль CMake

Этот раздел определяет синтаксис и имена, но не фиксирует устройство перечней исходников и модулей.

Команды CMake записываются строчными буквами и без пробела перед `(`:

```cmake
add_library(...)
target_link_libraries(...)
if (GRAPHICS_ENGINE_BUILD_SAMPLES)
    add_subdirectory(samples)
endif ()
```

Переменные и параметры проекта используют `UPPER_SNAKE_CASE`, общие параметры — префикс `GRAPHICS_ENGINE_`; избегать слишком общих `BUILD_SAMPLES`. Внутренние физические цели имеют имена проекта в `PascalCase`, открытые псевдонимы — пространство `GraphicsEngine::<Name>`.

В многострочных командах областей видимости `PUBLIC`, `PRIVATE` и `INTERFACE` ключи отступают на один уровень, значения — ещё на один. Такое же правило действует для `target_include_directories`. Механизм перечисления исходников и рекурсивных модулей регулируется отдельно.

### 11.1. Команды

Пример:

```cmake
add_library(...)
target_link_libraries(...)
if (...)
endif ()
```

### 11.2. Переменные и параметры

Пример:

```cmake
GRAPHICS_ENGINE_BUILD_SAMPLES
GRAPHICS_ENGINE_BUILD_TESTS
GRAPHICS_ENGINE_ENABLE_VULKAN
```

### 11.3. Цели сборки

Пример:

```cmake
add_library(GraphicsEngineCore STATIC ...)
add_library(GraphicsEngine::Core ALIAS GraphicsEngineCore)

add_library(GraphicsEngineVulkan STATIC ...)
add_library(GraphicsEngine::Vulkan ALIAS GraphicsEngineVulkan)
```

### 11.4. Многострочные вызовы

Пример:

```cmake
target_link_libraries(Target
    PUBLIC
    Foo
    Bar
    PRIVATE
    Baz
)
```

### 11.5. Условия

Допустимый вариант:

```cmake
if (GRAPHICS_ENGINE_BUILD_SAMPLES)
    add_subdirectory(samples)
endif ()
```

Не следует оставлять пустую ветвь:

```cmake
if (GRAPHICS_ENGINE_BUILD_SAMPLES)
endif ()
```

## 12. Форматирование и статические проверки

Стандартная версия — `clang-format 23.1.0`. Сценарии форматирования располагаются в `tools/formatter/` (например, `run.sh`, `check.sh` и `ignore`). Форматирование и статический анализ — разные задачи: форматтер меняет только оформление, а анализ считается проверкой и располагается в соответствующей инфраструктуре.

Сторонние, автоматически созданные и сборочные файлы не должны изменяться форматтером проекта. Типовые исключения: `external/`, `third_party/`, `build/`, `cmake-build-*/` и `generated/`. Точные правила исключений и порядок вызова определяет сам инструмент форматирования.

### Примеры раздела 12

Пример:

```text
clang-format 23.1.0
```

Пример:

```text
tools/
    formatter/
        run.sh
        check.sh
        ignore
        ...
```

Пример:

```text
external/
third_party/
build/
cmake-build-*/
generated/
```

