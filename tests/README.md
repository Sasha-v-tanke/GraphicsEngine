# Тесты

Тесты GraphicsEngine разделены на три уровня:

| Уровень  | Назначение                                                                                     | CMake preset     |
|----------|------------------------------------------------------------------------------------------------|------------------|
| `small`  | Быстрые проверки, не зависящие от graphics backend и window framework, Clang-Tidy и ShellCheck | `ci-lite`        |
| `medium` | Тесты C++ библиотек, Vulkan, GLFW и интеграций                                                 | `ci-vulkan-glfw` |
| `heavy`  | Дорогие runtime- и полные интеграционные тесты                                                 | `ci-vulkan-glfw` |

Проверки с санитайзерами используют backend-independent preset `ci-sanitizers` с включёнными ASan и UBSan.

## Запуск

Запустить уровень тестов из корня проекта:

```bash
./tests/test_small.sh
./tests/test_medium.sh
./tests/test_heavy.sh
./tests/run_sanitizers.sh
```

Отдельные группы тестов можно запускать через их локальные runner-скрипты, например:

```bash
./tests/medium/cpp/test_cpp.sh
./tests/medium/cpp/libs/test_libs.sh
```

В консоль выводится только краткий результат выполнения. Подробные логи сохраняются в:

```text
tests/result/
```

Для каждой операции в лог записываются время запуска, длительность и результат.

## CMake-конфигурации

Конфигурации сборки для CI определены в `CMakePresets.json`.

- `ci-lite` — тесты включены, Vulkan и GLFW выключены.
- `ci-vulkan-glfw` — тесты, Vulkan и GLFW включены.
- `ci-sanitizers` — основан на `ci-lite`, дополнительно включает ASan и UBSan.

В CI предупреждения компилятора считаются ошибками.

Test runner выбирает CMake preset и нужную группу CTest. CMake options не должны дублироваться в shell-скриптах.

## Добавление тестов

C++ тесты регистрируются через `test.cmake` и рекурсивно подключаются из `tests/tests.cmake`.

Каждому тесту назначаются CTest labels:

- `small`, `medium` или `heavy` — уровень теста;
- дополнительные labels, например `cpp` или `libs`, — группа теста.

Уровень определяет стоимость и область тестирования. CMake preset определяет конфигурацию сборки.

## CI

GitHub Actions запускает:

| Событие               | Small / Linux | ASan + UBSan / Linux | Medium / macOS | Heavy / macOS |
|-----------------------|--------------:|---------------------:|---------------:|--------------:|
| Pull Request в `main` |            ✓ |                   ✓ |             ✓ |             — |
| Push в `main`         |            ✓ |                   ✓ |             ✓ |            ✓ |
| Ручной запуск         |             — |                    — |              — |            ✓ |

Linux-проверки используют Clang 23.

На macOS собирается текущая поддерживаемая конфигурация Vulkan + GLFW.
