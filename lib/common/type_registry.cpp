#include "type_registry.h"

#include <algorithm>
#include <new>

namespace NCommon {

TypeDescriptor::TypeDescriptor(TypeId id,
                               std::string name,
                               std::uint32_t schemaVersion,
                               ETypeKind kind,
                               std::size_t size,
                               std::size_t alignment,
                               std::type_index cppType,
                               Operations operations,
                               std::optional<EnumTypeInfo> enumInfo,
                               std::vector<FieldMetadata> fields)
    : m_id(id)
    , m_name(std::move(name))
    , m_schemaVersion(schemaVersion)
    , m_kind(kind)
    , m_size(size)
    , m_alignment(alignment)
    , m_cppType(cppType)
    , m_operations(operations)
    , m_enumInfo(enumInfo)
    , m_fields(std::move(fields)) {
}

TypeId TypeDescriptor::GetId() const noexcept {
    return m_id;
}

std::string_view TypeDescriptor::GetName() const noexcept {
    return m_name;
}

std::uint32_t TypeDescriptor::GetSchemaVersion() const noexcept {
    return m_schemaVersion;
}

ETypeKind TypeDescriptor::GetKind() const noexcept {
    return m_kind;
}

std::size_t TypeDescriptor::GetSize() const noexcept {
    return m_size;
}

std::size_t TypeDescriptor::GetAlignment() const noexcept {
    return m_alignment;
}

TypeCapabilities TypeDescriptor::GetCapabilities() const noexcept {
    return {.DefaultConstructible = m_operations.Create != nullptr,
            .CopyConstructible = m_operations.Copy != nullptr,
            .MoveConstructible = m_operations.Move != nullptr,
            .Serializable = false};
}

std::optional<EnumTypeInfo> TypeDescriptor::GetEnumInfo() const noexcept {
    return m_enumInfo;
}

std::type_index TypeDescriptor::GetCppType() const noexcept {
    return m_cppType;
}

std::span<const FieldMetadata> TypeDescriptor::GetFields() const noexcept {
    return m_fields;
}

TypeValue::TypeValue(std::shared_ptr<const TypeDescriptor> descriptor, void* value)
    : m_descriptor(std::move(descriptor))
    , m_value(value) {
    // A class-specific noexcept operator new may return null instead of throwing.
    if (value == nullptr) {
        throw std::bad_alloc{};
    }
}

TypeValue::TypeValue(TypeValue&& other) noexcept
    : m_descriptor(std::move(other.m_descriptor))
    , m_value(std::exchange(other.m_value, nullptr)) {
}

TypeValue& TypeValue::operator=(TypeValue&& other) noexcept {
    if (this != &other) {
        Reset();
        m_descriptor = std::move(other.m_descriptor);
        m_value = std::exchange(other.m_value, nullptr);
    }
    return *this;
}

TypeValue::~TypeValue() {
    Reset();
}

bool TypeValue::IsValid() const noexcept {
    return m_value != nullptr;
}

const TypeDescriptor& TypeValue::GetType() const {
    if (!IsValid()) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_STATE, "TypeValue is empty");
    }
    return *m_descriptor;
}

void TypeValue::Reset() noexcept {
    // Empty the owner before invoking user destruction; retain its metadata through the call.
    if (auto* value = std::exchange(m_value, nullptr)) {
        auto descriptor = std::move(m_descriptor);
        descriptor->m_operations.Destroy(value);
    }
}

TypeValue TypeValue::Copy() const {
    const auto& descriptor = GetType();
    if (descriptor.m_operations.Copy == nullptr) {
        GRAPHICS_ENGINE_THROW(EError::UNSUPPORTED, "Type {} cannot be copied", descriptor.GetName());
    }
    return TypeValue{m_descriptor, descriptor.m_operations.Copy(m_value)};
}

TypeValue TypeValue::MoveValue() {
    const auto& descriptor = GetType();
    if (descriptor.m_operations.Move == nullptr) {
        GRAPHICS_ENGINE_THROW(EError::UNSUPPORTED, "Type {} cannot be move constructed", descriptor.GetName());
    }
    TypeValue moved{m_descriptor, descriptor.m_operations.Move(m_value)};
    Reset();
    return moved;
}

void TypeRegistry::ValidateOwner() const {
    if (std::this_thread::get_id() != m_owner) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_STATE, "TypeRegistry configuration requires its owner thread");
    }
}

void TypeRegistry::ValidateFrozen() const {
    if (!IsFrozen()) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_STATE, "TypeRegistry must be frozen before use");
    }
}

void TypeRegistry::RegisterDescriptor(TypeId id,
                                      std::string name,
                                      std::uint32_t schemaVersion,
                                      ETypeKind kind,
                                      std::size_t size,
                                      std::size_t alignment,
                                      std::type_index cppType,
                                      TypeDescriptor::Operations operations,
                                      std::optional<EnumTypeInfo> enumInfo,
                                      std::vector<FieldMetadata> fields) {
    ValidateOwner();
    if (IsFrozen()) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_STATE, "TypeRegistry registration is closed");
    }
    if (!id.IsValid() || name.empty() || name.find('\0') != std::string::npos || schemaVersion == 0) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_ARGUMENT, "Type ID, schema version and name must be valid and nonzero");
    }
    switch (kind) {
    case ETypeKind::VALUE:
    case ETypeKind::COMPONENT:
        if (enumInfo.has_value()) {
            GRAPHICS_ENGINE_THROW(EError::INVALID_ARGUMENT, "Enum types require ENUM metadata");
        }
        break;
    case ETypeKind::ENUM:
        if (!enumInfo.has_value()) {
            GRAPHICS_ENGINE_THROW(EError::INVALID_ARGUMENT, "ENUM metadata requires a C++ enum type");
        }
        break;
    default:
        GRAPHICS_ENGINE_THROW(EError::INVALID_ARGUMENT, "Unknown metadata type kind");
    }
    if (enumInfo && !fields.empty()) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_ARGUMENT, "Enum metadata cannot declare object fields");
    }
    std::ranges::sort(fields, {}, [](const FieldMetadata& field) { return field.Id.Value; });
    for (std::size_t index = 0; index < fields.size(); ++index) {
        const auto& field = fields[index];
        if (field.Id.Value == 0 || field.Name.empty() || field.Name.find('\0') != std::string::npos ||
            field.CppType == typeid(void) || field.OwnerType != cppType) {
            GRAPHICS_ENGINE_THROW(EError::INVALID_ARGUMENT, "Invalid field declaration for type {}", name);
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (fields[previous].Id == field.Id || fields[previous].Name == field.Name) {
                GRAPHICS_ENGINE_THROW(EError::ALREADY_EXISTS, "Duplicate property ID or name in type {}", name);
            }
        }
    }
    for (const auto& existing: m_types) {
        if (existing->GetId() == id || existing->GetCppType() == cppType || existing->GetName() == name) {
            if (existing->GetSchemaVersion() != schemaVersion) {
                GRAPHICS_ENGINE_THROW(EError::INVALID_ARGUMENT,
                                      "Schema version mismatch for type {} (ID {}): {} vs {}",
                                      name,
                                      id.Value,
                                      existing->GetSchemaVersion(),
                                      schemaVersion);
            }
            GRAPHICS_ENGINE_THROW(EError::ALREADY_EXISTS,
                                  "Duplicate type ID, C++ type or name: {} (ID {})",
                                  name,
                                  id.Value);
        }
    }
    auto descriptor = std::shared_ptr<const TypeDescriptor>{new TypeDescriptor{id,
                                                                               std::move(name),
                                                                               schemaVersion,
                                                                               kind,
                                                                               size,
                                                                               alignment,
                                                                               cppType,
                                                                               operations,
                                                                               enumInfo,
                                                                               std::move(fields)}};
    m_types.push_back(std::move(descriptor));
}

void TypeRegistry::Freeze() {
    ValidateOwner();
    if (!IsFrozen()) {
        std::ranges::sort(m_types, {}, [](const auto& descriptor) { return descriptor->GetId().Value; });
        m_frozen.store(true, std::memory_order_release);
    }
}

bool TypeRegistry::IsFrozen() const noexcept {
    return m_frozen.load(std::memory_order_acquire);
}

std::shared_ptr<const TypeDescriptor> TypeRegistry::Find(TypeId id) const {
    ValidateFrozen();
    for (const auto& descriptor: m_types) {
        if (descriptor->GetId() == id) {
            return descriptor;
        }
    }
    return {};
}

std::shared_ptr<const TypeDescriptor> TypeRegistry::Find(std::string_view name) const {
    ValidateFrozen();
    for (const auto& descriptor: m_types) {
        if (descriptor->GetName() == name) {
            return descriptor;
        }
    }
    return {};
}

std::shared_ptr<const TypeDescriptor> TypeRegistry::FindCppType(std::type_index type) const {
    ValidateFrozen();
    for (const auto& descriptor: m_types) {
        if (descriptor->GetCppType() == type) {
            return descriptor;
        }
    }
    return {};
}

std::shared_ptr<const TypeDescriptor> TypeRegistry::Get(TypeId id) const {
    if (auto descriptor = Find(id)) {
        return descriptor;
    }
    GRAPHICS_ENGINE_THROW(EError::NOT_FOUND, "No metadata registered for type ID {}", id.Value);
}

std::span<const std::shared_ptr<const TypeDescriptor>> TypeRegistry::GetTypes() const {
    ValidateFrozen();
    return m_types;
}

TypeValue TypeRegistry::Create(TypeId id) const {
    auto descriptor = Get(id);
    if (descriptor->m_operations.Create == nullptr) {
        GRAPHICS_ENGINE_THROW(EError::UNSUPPORTED, "Type {} is not default constructible", descriptor->GetName());
    }
    auto* value = descriptor->m_operations.Create();
    return TypeValue{std::move(descriptor), value};
}

} // namespace NCommon
