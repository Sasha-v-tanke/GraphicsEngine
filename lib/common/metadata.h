#pragma once

#include <string>
#include <type_traits>
#include <utility>

#include <GraphicsEngine/lib/common/type_registry.h>

namespace NCommon {

namespace NMetadataInternal {

template<typename T>
struct MemberTraits;

template<typename Owner, typename Field>
struct MemberTraits<Field Owner::*> {
    using OwnerType = Owner;
    using FieldType = Field;
};

} // namespace NMetadataInternal

// Portable opt-in annotation usable in handwritten registrations and generated code.
// Taking a member address validates the C++ declaration, but the pointer is never retained.
template<typename Owner, auto Member, std::uint64_t Id>
[[nodiscard]] FieldMetadata MakeFieldMetadata(std::string name) {
    static_assert(Id != 0, "metadata property ID must be nonzero");
    static_assert(std::is_member_object_pointer_v<decltype(Member)>, "metadata requires a non-static data member");
    if constexpr (std::is_member_object_pointer_v<decltype(Member)>) {
        using Traits = NMetadataInternal::MemberTraits<decltype(Member)>;
        static_assert(std::is_same_v<Owner, typename Traits::OwnerType>, "metadata member owner mismatch");
        return {.Id = {Id},
                .Name = std::move(name),
                .CppType = typeid(typename Traits::FieldType),
                .OwnerType = typeid(Owner)};
    }
}

} // namespace NCommon

#define GRAPHICS_ENGINE_METADATA_FIELD(Type, Member, Id, Name)                                                         \
    ::NCommon::MakeFieldMetadata<Type, &Type::Member, Id>(Name)
