# Генерация opt-in metadata

`FEATURE-0107` добавляет offline build step поверх `NCommon::TypeRegistry`. Декларации — явные
JSON sidecars, а не разбор произвольного C++ текста. Генератор использует только Python 3.9+ и стандартную
библиотеку; runtime, обычные C++ компоненты и `find_package(GraphicsEngine)` не требуют Python.
Он обнаруживается только при явном вызове `GRAPHICS_ENGINE_GENERATE_METADATA`.

```cmake
find_package(GraphicsEngine REQUIRED CONFIG)
add_library(Gameplay STATIC gameplay.cpp)
target_link_libraries(Gameplay PUBLIC GraphicsEngine::GraphicsEngine)
target_include_directories(Gameplay PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
GRAPHICS_ENGINE_GENERATE_METADATA(
    TARGET Gameplay
    FUNCTION NGame::RegisterMetadata
    SCHEMAS gameplay.meta.json
)
```

Пример `gameplay.meta.json` для публичного `NGame::Position` из `game/position.h`:

```json
{
  "format_version": 1,
  "types": [{
    "id": "0x1001",
    "cpp_type": "::NGame::Position",
    "name": "Game.Position",
    "version": 1,
    "kind": "COMPONENT",
    "header": "game/position.h",
    "fields": [{"id": 11, "member": "X", "name": "Horizontal", "cpp_type": "float"}]
  }]
}
```

Обязательны explicit ненулевые TypeId/PropertyId (числа JSON либо десятичные/hex строки) и версия
схемы. TypeId уникальны среди всех переданных sidecars; PropertyId — в пределах своего TypeId. Переименование
`member`/`name`, перестановка деклараций и перемещение файлов не вычисляют новые ID. Автор сохраняет их явно.
Дубликаты ID, C++ типов, имён, полей и JSON ключей, неизвестные ключи/версии формата и недопустимые значения
отклоняются до записи результатов. Имена могут быть printable Unicode; header — переносимый относительный include
path без `..`. `cpp_type` — C++ type expression; используйте полные имена `::NGame::Type`, не зависящие от namespace
функции регистрации. Полноту и корректность типов проверяет C++ компилятор, не Python.

В binary tree создаются `metadata/Gameplay/Gameplay.gen.h`, `.gen.cpp`, `.manifest.json` и stamp успешной
генерации. Header объявляет `NGame::RegisterMetadata(TypeRegistry&)`; generated cpp подключается к target.
Вызывайте эту функцию явно, вместе с регистрациями других библиотек, затем один раз `Freeze()`.
Автоматических статических registrars и глобального singleton нет. Разные target имеют отдельные output directories
и функции регистрации; конфликт ID между независимо собранными библиотеками отклоняет общий TypeRegistry при startup.
Для ранней проверки общего набора передайте все его sidecars в одну генерацию.

Генератор сортирует типы/поля по ID, headers по имени; canonical manifest не содержит абсолютных путей,
часов, случайных ID или регистрационного порядка. Неизменившиеся файлы не перезаписываются. Build graph задаёт
явные schemas/tool dependencies и stamp только после успешного завершения; изменения JSON/tool инвалидируют
свою генерацию. Обычные header changes отслеживаются компилятором. Ручной CLI имеет параметры `--schema` (повторяемый),
`--function`, `--header`, `--source`, `--manifest`.

`<GraphicsEngine/lib/common/metadata.h>` предоставляет portable macro
`GRAPHICS_ENGINE_METADATA_FIELD(Owner, Member, PropertyId, Name)` для generated и ручных регистраций.
Compile-time проверки требуют ненулевой ID, существующее non-static data member, точный owner и объявленный
тип поля. Закрытые/статические поля, методы, bitfields и несовместимый тип не становятся скрытыми raw accessors.
В `TypeDescriptor::GetFields()` публикуются только immutable ID/name/CppType/OwnerType, отсортированные по ID;
registry дополнительно отклоняет foreign owners, нулевые/повторные ID, повторные имена и enum object fields.
Указатели на члены/объекты не сохраняются. Getter/setter, property flags, codecs и доступ к live ECS принадлежат
следующим задачам; наличие declarations не включает `Serializable`.

CMake helper и generator устанавливаются рядом с package config; downstream потребитель получает ту же команду
после `find_package`. Generated user headers доступны через BUILD_INTERFACE target; install/export собственных
пользовательских libraries/headers задаёт их владелец. Шаг не создаёт второй Asset Pipeline, DDC, cooker или
runtime scheduler: он остаётся предметным producer metadata Plan II, с explicit inputs/outputs и обычными targets.
