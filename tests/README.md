# Проверки проекта

Тесты GraphicsEngine разделены на три уровня:

| Уровень | Назначение | Конфигурация CMake |
| --- | --- | --- |
| `small` | Быстрые проверки без графической реализации и оконной библиотеки, Clang-Tidy и ShellCheck | `ci-lite` |
| `medium` | Проверки библиотек C++, Vulkan, GLFW и интеграций | `ci-vulkan-glfw` |
| `heavy` | Ресурсоёмкие проверки среды исполнения и полные интеграционные проверки | `ci-vulkan-glfw` |

Для проверок с санитайзерами используется независимая от графического устройства конфигурация `ci-sanitizers` с ASan и UBSan.

## Запуск

Из корня проекта:

```bash
./tests/test_small.sh
./tests/test_medium.sh
./tests/test_heavy.sh
./tests/run_sanitizers.sh
```

Отдельные группы запускаются соответствующими сценариями, например:

```bash
./tests/medium/cpp/test_cpp.sh
./tests/medium/cpp/libs/test_libs.sh
```

В консоли выводится краткий результат. Подробные журналы с временем запуска, длительностью и результатом сохраняются в `tests/result/`.

## Конфигурации CMake

Конфигурации сборки определены в `CMakePresets.json`:

- `ci-lite` — тесты включены, Vulkan и GLFW выключены;
- `ci-vulkan-glfw` — тесты, Vulkan и GLFW включены;
- `ci-sanitizers` — основана на `ci-lite`, дополнительно включает ASan и UBSan.

В проверочных конфигурациях предупреждения компилятора считаются ошибками. Сценарий запуска выбирает конфигурацию CMake и группу CTest; параметры CMake не дублируются в сценариях оболочки.

## Добавление тестов

### Окна без захвата фокуса

`TEST_SUITE` добавляет `tests/common/process_bootstrap.cpp` непосредственно в каждый тестовый
executable, в том числе с собственным `main`. До входа в `main` bootstrap устанавливает внутренний
контракт `GRAPHICS_ENGINE_INTERNAL_TEST_NON_ACTIVATING=1`. Ручная настройка окружения не нужна:
прямой запуск, `--gtest_filter`, CTest и сборки с санитайзерами используют один путь. `posix_spawn`
smoke-тесты Vulkan наследуют окружение родителя. В тестах без окон bootstrap только задаёт контракт;
он не инициализирует GLFW, Cocoa или `WindowRuntime`. Создавать окна в глобальных конструкторах
нельзя: оконная среда создаётся в основном потоке после входа в `main`.

GLFW backend перед каждым созданием тестового окна задаёт `GLFW_FOCUSED=false` и
`GLFW_FOCUS_ON_SHOW=false`. На macOS до `glfwInit` отключается автоматическое создание меню и
перевод небандлированного процесса в foreground application. Окна остаются видимыми, surface,
swapchain, resize и present сохраняются. Это частный контракт тестовой инфраструктуры, не параметр
`WindowConfig` или `ApplicationConfig`. Обычные приложения и samples его не устанавливают.

Покрываемые процессы: `WindowFocus`, Vulkan surface/device/swapchain/frame/submission (включая
дочерние smoke), `ApplicationPresentation` и `GraphPresentation`; общие сценарии samples,
вызываемые внутри тестов, наследуют тот же процесс. `Window` с fake backend и остальные CPU suites
не создают окон. Focus/input проверяются управляемыми событиями/fake backend; автоматический suite
не вызывает `glfwFocusWindow` и не отключает контракт ради input-теста.

`GraphicsEngineWindowFocusTests` проверяет видимость и отсутствие фокуса при создании нескольких
окон, show, resize, закрытии и повторной инициализации GLFW. Проверка намеренно меняет глобальные
focus hints перед созданием следующего окна. Недоступность display при первой инициализации —
явный SKIP; последующие ошибки не маскируются.

На macOS дополнительно собирается `GraphicsEngineCheckWindowFocus`: отдельный процесс опрашивает
PID frontmost application каждую миллисекунду от запуска команды до её завершения, включая дочерние
процессы. Он не активирует приложения и не требует Accessibility-разрешений. Любая наблюдаемая
смена PID или ошибка команды означает FAIL; отсутствие GUI-сессии — код 77 / SKIP. CTest
`GraphicsEngineWindowFrontmostProcess` автоматически оборачивает оконную регрессию этим наблюдателем.

Для проверки реального GPU/WSI пути оставьте IDE/терминал активным и не переключайте приложения
во время команды. После обычной настройки Vulkan loader/ICD для системы выполните:

```bash
build/cmake/GraphicsEngineCheckWindowFocus build/cmake/GraphicsEngineWindowFocusTests
build/cmake/GraphicsEngineCheckWindowFocus build/cmake/GraphicsEngineWindowFocusTests --gtest_filter=WindowFocus.* --gtest_repeat=5
build/cmake/GraphicsEngineCheckWindowFocus ctest --test-dir build/cmake -L heavy --output-on-failure
ctest --test-dir build/cmake -R 'Window(FocusTests|FrontmostProcess)' --output-on-failure
```

Замените `build/cmake` каталогом своей сборки `ci-vulkan-glfw`. Наблюдатель также возвращает ошибку,
если пользователь сам переключит приложение; такой прогон нужно повторить без переключений.
Опрос фиксирует наблюдаемые смены PID, но не гарантирует обнаружение перехода короче 1 мс.
Обычный sample проверяется отдельно без тестового окружения; он сохраняет стандартные focus hints.

Тесты C++ регистрируются через `test.cmake` и рекурсивно подключаются из `tests/tests.cmake`. Каждому назначаются метки CTest:

- `small`, `medium` или `heavy` — стоимость и область проверки;
- дополнительные метки, например `cpp` или `libs`, — группа.

Уровень определяет стоимость тестирования, а конфигурация CMake — параметры сборки.

## Непрерывная интеграция

Рабочие процессы GitHub Actions настроены следующим образом:

| Событие | Малые / Linux | ASan + UBSan / Linux | Средние / macOS | Тяжёлые / macOS |
| --- | ---: | ---: | ---: | ---: |
| Запрос на включение изменений в `main` | ✓ | ✓ | ✓ | — |
| Отправка изменений в `main` | ✓ | ✓ | ✓ | ✓ |
| Ручной запуск | — | — | — | ✓ |

В Linux-проверках используется Clang 23. На macOS собирается текущая конфигурация Vulkan и GLFW.
