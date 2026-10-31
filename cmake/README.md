# Средства сборки CMake

`utils.cmake` предоставляет компактный набор команд для объявления модулей GraphicsEngine, тестов, измерений производительности, примеров, исходных файлов и зависимостей.

Публичные команды:

```cmake
MODULE(name)
SUBMODULE()
API()

TEST_SUITE(name)
TEST(name)
TEST()

BENCHMARK_SUITE(name)
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

Тесты и измерения не выводят зависимости от производственных модулей по имени. Все нужные модули перечисляются явно через `PRIVATE_DEPENDS(...)`.

## Входные файлы

Каталог, участвующий в сборке, использует один входной файл из списка:

```text
module.cmake
submodule.cmake
tests.cmake
test.cmake
benchmarks.cmake
benchmark.cmake
sample.cmake
```

`RECURSE()` автоматически обнаруживает входной файл. В каждом каталоге должен быть ровно один поддерживаемый файл.

## Модули библиотеки

Объявление модуля:

```cmake
MODULE(Window)

API(
    window.h
)

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

Создаются `GraphicsEngineWindow` и `GraphicsEngine::Window`. Модули библиотеки являются статическими. Так сохраняется смысл `PRIVATE_DEPENDS` независимо от `BUILD_SHARED_LIBS`: закрытые зависимости остаются необходимыми при связывании установленного потребителя, но не расширяют публичный интерфейс заголовков.

Дополнение существующего модуля из вложенного каталога:

```cmake
SUBMODULE()

SOURCES(
    engine.cpp
)
```

`SUBMODULE()` не создаёт отдельную цель. Он добавляет исходники и локальные заголовки к текущему модулю.

## Тесты

Объявление набора тестов:

```cmake
TEST_SUITE(Window)

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

Создаются `GraphicsEngineWindowTests` и `GraphicsEngine::WindowTests`. `TEST_SUITE(name)` создаёт исполняемый файл, `TEST(name)` открывает логическую группу внутри набора, а `TEST()` во вложенном каталоге продолжает активную группу.

Отдельные проверки используют ту же структуру с собственным набором, например `TEST_SUITE(CommonLibs)` и явными зависимостями `GLM`, `StbImage`, `TinyObj`.

Метки тестов являются метками CTest. Сценарии запуска и пользовательские цели используют их, чтобы выбирать, например, все `small`, все `libs` или тесты конкретного каталога. `TEST_LABELS(...)` автоматически добавляет:

```text
dir:<relative-source-directory>
test:<current-test-name>
```

Файлы `.cpp` производственных модулей не добавляются повторно в тестовые цели: тесты используют код через зависимости модулей.

## Измерения производительности

Набор объявляется `BENCHMARK_SUITE(name)`, логическая группа — `BENCHMARK(name)`, а `BENCHMARK()` во вложенном каталоге продолжает текущую группу. `BENCHMARK_LABELS(...)` добавляет метки, включая:

```text
dir:<relative-source-directory>
benchmark:<current-benchmark-name>
```

Исполняемый файл создаёт `BENCHMARK_SUITE`. Обычно группы выбираются во время выполнения через параметры запуска, например `--benchmark_filter=Smoke`.

## Примеры

Объявление исполняемого примера:

```cmake
SAMPLE(Base)

SOURCES(
    main.cpp
)

PRIVATE_DEPENDS(
    Window
)
```

Отдельной команды `SAMPLE_PART()` нет: она может появиться только при реальной необходимости более сложной структуры примеров.

## Исходные файлы и заголовки

Файлы `.cpp` всегда перечисляются явно:

```cmake
SOURCES(
    window.cpp
    factory.cpp
)
```

`MODULE()` и `SUBMODULE()` автоматически собирают локальные файлы `.h` как закрытые исходники цели. Открытые заголовки модуля явно перечисляются через `API(...)` в корневом `module.cmake`. Остальные заголовки являются внутренними.

```cmake
API(
    window.h
    window_config.h
    window_size.h
    window_type.h
)
```

## Зависимости

Внутренние зависимости:

```cmake
PRIVATE_DEPENDS(
    Vulkan::Vulkan
    Threads::Threads
)
```

Зависимости публичного интерфейса:

```cmake
PUBLIC_DEPENDS(
    Common
)
```

Короткие имена проекта разрешаются автоматически: `Common` → `GraphicsEngine::Common`, `Window` → `GraphicsEngine::Window`. Полные имена целей CMake, такие как `Vulkan::Vulkan`, `Threads::Threads` и `GTest::gtest_main`, не изменяются.

Вспомогательные команды покрывают только общие операции проекта. За пределами этой абстракции следует использовать обычный CMake напрямую.

## Внутренняя организация

`cmake/utils.cmake` — общая точка подключения. Реализация разделена по ответственности:

```text
cmake/utils/context.cmake        # контекст и локальные заголовки
cmake/utils/target_options.cmake # общие настройки целей
cmake/utils/dependencies.cmake   # разрешение зависимостей и связывание
cmake/utils/sources.cmake        # SOURCES(...)
cmake/utils/modules.cmake        # MODULE(...), SUBMODULE()
cmake/utils/tests.cmake          # TEST_SUITE(...), TEST(...), TEST_LABELS(...), TEST_TARGET(...)
cmake/utils/benchmarks.cmake     # BENCHMARK_SUITE(...), BENCHMARK(...), BENCHMARK_LABELS(...)
cmake/utils/samples.cmake        # SAMPLE(...)
cmake/utils/recurse.cmake        # RECURSE(...) и обнаружение входных файлов
cmake/utils/external.cmake       # include_external(...)
cmake/utils/package_api.cmake    # PACKAGE_ROOT(...), PACKAGE_COMPONENT(...), PACKAGE_DEPENDS(...)
```

Файлы в `cmake/utils/` — внутренние детали набора команд. Входные файлы модулей подключают `cmake/utils.cmake` через конфигурацию корневого проекта, но не включают отдельные внутренние файлы напрямую.

## Устанавливаемый пакет

GraphicsEngine можно установить как пакет CMake. Публичная цель подключения — `GraphicsEngine::GraphicsEngine`; внутренние модули не входят в публичный интерфейс CMake.

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

Если необходима конкретная возможность установленной сборки:

```cmake
find_package(
    GraphicsEngine REQUIRED CONFIG
    COMPONENTS Vulkan GLFW
)
```

Компоненты проверяют возможности установленной сборки и не добавляют отдельных публичных целей.
