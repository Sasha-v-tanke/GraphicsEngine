#pragma once

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <vector>

#include <GraphicsEngine/lib/common/error/error.h>
#include <GraphicsEngine/lib/common/error/exception.h>
#include <GraphicsEngine/lib/common/wrapper/non_copyable.h>
#include <GraphicsEngine/lib/common/wrapper/non_transferable.h>

namespace NCommon {

// Assigned by the schema author. Zero is invalid; never derive this value from RTTI or registration order.
struct TypeId {
    std::uint64_t Value = 0;
    [[nodiscard]] bool IsValid() const noexcept {
        return Value != 0;
    }
    bool operator==(const TypeId&) const noexcept = default;
};

enum class ETypeKind {
    VALUE,
    COMPONENT,
    ENUM,
};

struct TypeCapabilities {
    bool DefaultConstructible = false;
    bool CopyConstructible = false;
    bool MoveConstructible = false;
    // Metadata registration alone does not provide a codec.
    bool Serializable = false;
};

struct EnumTypeInfo {
    std::size_t UnderlyingSize = 0;
    bool IsSigned = false;
};

// Stable within its declaring TypeId; renaming/reordering a C++ member does not change this ID.
struct PropertyId {
    std::uint64_t Value = 0;
    bool operator==(const PropertyId&) const noexcept = default;
};

// Declaration metadata only. No live component pointer, getter, setter or serialization codec.
struct FieldMetadata {
    PropertyId Id;
    std::string Name;
    std::type_index CppType = typeid(void);
    std::type_index OwnerType = typeid(void);
};

class TypeRegistry;
class TypeValue;

// Immutable metadata, shared independently of the registry and any World.
class TypeDescriptor final: private NonTransferable {
public:
    [[nodiscard]] TypeId GetId() const noexcept;
    [[nodiscard]] std::string_view GetName() const noexcept;
    [[nodiscard]] std::uint32_t GetSchemaVersion() const noexcept;
    [[nodiscard]] ETypeKind GetKind() const noexcept;
    [[nodiscard]] std::size_t GetSize() const noexcept;
    [[nodiscard]] std::size_t GetAlignment() const noexcept;
    [[nodiscard]] TypeCapabilities GetCapabilities() const noexcept;
    [[nodiscard]] std::optional<EnumTypeInfo> GetEnumInfo() const noexcept;
    // An in-process binding only. It is never the persistent identity.
    [[nodiscard]] std::type_index GetCppType() const noexcept;
    [[nodiscard]] std::span<const FieldMetadata> GetFields() const noexcept;

private:
    friend class TypeRegistry;
    friend class TypeValue;

    struct Operations {
        void* (*Create)() = nullptr;
        void* (*Copy)(const void*) = nullptr;
        void* (*Move)(void*) = nullptr;
        void (*Destroy)(void*) noexcept = nullptr;
    };

    TypeDescriptor(TypeId id,
                   std::string name,
                   std::uint32_t schemaVersion,
                   ETypeKind kind,
                   std::size_t size,
                   std::size_t alignment,
                   std::type_index cppType,
                   Operations operations,
                   std::optional<EnumTypeInfo> enumInfo,
                   std::vector<FieldMetadata> fields);

private:
    TypeId m_id;
    std::string m_name;
    std::uint32_t m_schemaVersion;
    ETypeKind m_kind;
    std::size_t m_size;
    std::size_t m_alignment;
    std::type_index m_cppType;
    Operations m_operations;
    std::optional<EnumTypeInfo> m_enumInfo;
    std::vector<FieldMetadata> m_fields;
};

// Owns a standalone value and its immutable metadata, never a component pointer into a World.
// Destruction uses the matching typed delete, including over-aligned and nontrivial types.
class TypeValue final: private NonCopyable {
public:
    TypeValue() = default;
    TypeValue(TypeValue&& other) noexcept;
    TypeValue& operator=(TypeValue&& other) noexcept;
    ~TypeValue();

    [[nodiscard]] bool IsValid() const noexcept;
    [[nodiscard]] const TypeDescriptor& GetType() const;
    void Reset() noexcept;
    [[nodiscard]] TypeValue Copy() const;
    // Constructs a new T from T&&, then destroys/empties this value on success.
    // A throwing move constructor leaves this owner alive with T's own exception guarantee.
    [[nodiscard]] TypeValue MoveValue();

    template<typename T>
    [[nodiscard]] T* TryGet() noexcept {
        return m_descriptor && m_descriptor->GetCppType() == typeid(T) ? static_cast<T*>(m_value) : nullptr;
    }

    template<typename T>
    [[nodiscard]] const T* TryGet() const noexcept {
        return m_descriptor && m_descriptor->GetCppType() == typeid(T) ? static_cast<const T*>(m_value) : nullptr;
    }

    template<typename T>
    [[nodiscard]] T& Get() {
        if (auto* value = TryGet<T>()) {
            return *value;
        }
        GRAPHICS_ENGINE_THROW(EError::INVALID_ARGUMENT, "TypeValue is empty or has a different C++ type");
    }

    template<typename T>
    [[nodiscard]] const T& Get() const {
        if (const auto* value = TryGet<T>()) {
            return *value;
        }
        GRAPHICS_ENGINE_THROW(EError::INVALID_ARGUMENT, "TypeValue is empty or has a different C++ type");
    }

private:
    friend class TypeRegistry;
    TypeValue(std::shared_ptr<const TypeDescriptor> descriptor, void* value);

private:
    std::shared_ptr<const TypeDescriptor> m_descriptor;
    void* m_value = nullptr;
};

// Configuration belongs to the creating thread. Freeze publishes metadata for concurrent read-only use.
// Destruction must not race with registry calls; retained descriptors/TypeValues may outlive this object.
class TypeRegistry final: private NonTransferable {
public:
    TypeRegistry() = default;

    // T remains an ordinary opt-in C++ type; no base class or ECS registration is required.
    template<typename T>
        requires(std::same_as<T, std::remove_cvref_t<T>> && std::is_object_v<T> && !std::is_array_v<T> &&
                 std::destructible<T>)
    void Register(TypeId id,
                  std::string name,
                  std::uint32_t schemaVersion = 1,
                  ETypeKind kind = std::is_enum_v<T> ? ETypeKind::ENUM : ETypeKind::VALUE,
                  std::vector<FieldMetadata> fields = {}) {
        TypeDescriptor::Operations operations;
        if constexpr (std::is_default_constructible_v<T>) {
            operations.Create = []() -> void* { return new T(); };
        }
        if constexpr (std::is_copy_constructible_v<T>) {
            operations.Copy = [](const void* source) -> void* { return new T(*static_cast<const T*>(source)); };
        }
        if constexpr (std::is_move_constructible_v<T>) {
            operations.Move = [](void* source) -> void* { return new T(std::move(*static_cast<T*>(source))); };
        }
        operations.Destroy = [](void* value) noexcept { delete static_cast<T*>(value); };
        std::optional<EnumTypeInfo> enumInfo;
        if constexpr (std::is_enum_v<T>) {
            using Underlying = std::underlying_type_t<T>;
            enumInfo = {.UnderlyingSize = sizeof(Underlying), .IsSigned = std::is_signed_v<Underlying>};
        }
        RegisterDescriptor(id,
                           std::move(name),
                           schemaVersion,
                           kind,
                           sizeof(T),
                           alignof(T),
                           typeid(T),
                           operations,
                           enumInfo,
                           std::move(fields));
    }

    // Idempotent on the owner. Enumerations are sorted by explicit ID, independent of module registration order.
    void Freeze();
    [[nodiscard]] bool IsFrozen() const noexcept;

    // All lookup/creation methods require Freeze. Find returns nullptr for an unknown type.
    [[nodiscard]] std::shared_ptr<const TypeDescriptor> Find(TypeId id) const;
    [[nodiscard]] std::shared_ptr<const TypeDescriptor> Find(std::string_view name) const;
    template<typename T>
    [[nodiscard]] std::shared_ptr<const TypeDescriptor> Find() const {
        return FindCppType(typeid(T));
    }
    [[nodiscard]] std::shared_ptr<const TypeDescriptor> Get(TypeId id) const;
    template<typename T>
    [[nodiscard]] std::shared_ptr<const TypeDescriptor> Get() const {
        if (auto descriptor = Find<T>()) {
            return descriptor;
        }
        GRAPHICS_ENGINE_THROW(EError::NOT_FOUND, "No metadata registered for C++ type {}", typeid(T).name());
    }
    // Borrowed container view; copy a descriptor's shared_ptr to retain it across registry destruction.
    [[nodiscard]] std::span<const std::shared_ptr<const TypeDescriptor>> GetTypes() const;
    [[nodiscard]] TypeValue Create(TypeId id) const;

    template<typename T, typename... Args>
        requires(std::same_as<T, std::remove_cvref_t<T>> && std::constructible_from<T, Args...> && !std::is_array_v<T>)
    [[nodiscard]] TypeValue Make(Args&&... args) const {
        auto descriptor = Get<T>();
        return TypeValue{std::move(descriptor), new T(std::forward<Args>(args)...)};
    }

private:
    void ValidateOwner() const;
    void ValidateFrozen() const;
    [[nodiscard]] std::shared_ptr<const TypeDescriptor> FindCppType(std::type_index type) const;
    void RegisterDescriptor(TypeId id,
                            std::string name,
                            std::uint32_t schemaVersion,
                            ETypeKind kind,
                            std::size_t size,
                            std::size_t alignment,
                            std::type_index cppType,
                            TypeDescriptor::Operations operations,
                            std::optional<EnumTypeInfo> enumInfo,
                            std::vector<FieldMetadata> fields);

private:
    const std::thread::id m_owner = std::this_thread::get_id();
    std::vector<std::shared_ptr<const TypeDescriptor>> m_types;
    std::atomic_bool m_frozen = false;
};

} // namespace NCommon
