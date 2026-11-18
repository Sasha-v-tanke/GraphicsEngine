#include "type_registry_modules.h"

#include <array>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>
#include <tests/common/test_error.h>

namespace {

using NCommon::EError;
using NCommon::ETypeKind;
using NCommon::TypeId;
using NCommon::TypeRegistry;
using NCommon::TypeValue;
using NTypeRegistryTest::EMode;
using NTypeRegistryTest::Position;
using NTypeRegistryTest::RegisterAssets;
using NTypeRegistryTest::RegisterGameplay;

static_assert(!std::is_move_constructible_v<TypeRegistry>);
static_assert(!std::is_copy_constructible_v<TypeValue>);
static_assert(std::is_nothrow_move_constructible_v<TypeValue>);
static_assert(std::is_nothrow_move_assignable_v<TypeValue>);
static_assert(std::same_as<decltype(std::declval<const TypeValue&>().Get<int>()), const int&>);

TEST(TypeRegistry, IndependentModulesHaveStableExplicitIdentityAndCanonicalEnumeration) {
    TypeRegistry first;
    RegisterGameplay(first);
    RegisterAssets(first);
    first.Freeze();
    TypeRegistry second;
    RegisterAssets(second);
    RegisterGameplay(second);
    second.Freeze();
    const auto left = first.GetTypes();
    const auto right = second.GetTypes();
    ASSERT_EQ(left.size(), 3);
    ASSERT_EQ(right.size(), 3);
    for (std::size_t index = 0; index < left.size(); ++index) {
        EXPECT_EQ(left[index]->GetId(), right[index]->GetId());
        EXPECT_EQ(left[index]->GetName(), right[index]->GetName());
        EXPECT_EQ(left[index]->GetSchemaVersion(), right[index]->GetSchemaVersion());
        EXPECT_EQ(left[index]->GetId().Value, 1001 + index);
    }
    const auto position = first.Get<Position>();
    EXPECT_EQ(NTypeRegistryTest::FindPositionFromOtherModule(first), position);
    EXPECT_EQ(first.Find("Game.Position"), position);
    EXPECT_EQ(first.Get(TypeId{1002}), position);
    EXPECT_EQ(position->GetKind(), ETypeKind::COMPONENT);
    EXPECT_EQ(position->GetSchemaVersion(), 2);
    EXPECT_EQ(position->GetSize(), sizeof(Position));
    EXPECT_EQ(position->GetAlignment(), alignof(Position));
    EXPECT_EQ(position->GetCppType(), typeid(Position));
    EXPECT_FALSE(position->GetEnumInfo().has_value());
}

TEST(TypeRegistry, RejectsDuplicateCppTypeIdNameAndSchemaMismatchWithoutPartialPublication) {
    TypeRegistry registry;
    registry.Register<int>({1}, "Core.Int", 2);
    NTest::ExpectError(EError::ALREADY_EXISTS, [&] { registry.Register<int>({2}, "Core.OtherInt", 2); });
    NTest::ExpectError(EError::ALREADY_EXISTS, [&] { registry.Register<float>({1}, "Core.Float", 2); });
    NTest::ExpectError(EError::ALREADY_EXISTS, [&] { registry.Register<float>({2}, "Core.Int", 2); });
    NTest::ExpectError(EError::INVALID_ARGUMENT, [&] { registry.Register<int>({1}, "Core.Int", 3); });
    NTest::ExpectError(EError::INVALID_ARGUMENT, [&] { registry.Register<float>({1}, "Core.Float", 3); });
    NTest::ExpectError(EError::INVALID_ARGUMENT, [&] { registry.Register<float>({2}, "Core.Int", 3); });
    registry.Register<float>({2}, "Core.Float", 2);
    registry.Freeze();
    ASSERT_EQ(registry.GetTypes().size(), 2);
    EXPECT_EQ(registry.Get<int>()->GetId(), TypeId{1});
    EXPECT_EQ(registry.Get<float>()->GetId(), TypeId{2});
}

TEST(TypeRegistry, ValidatesMetadataBeforePublishingIt) {
    TypeRegistry registry;
    NTest::ExpectError(EError::INVALID_ARGUMENT, [&] { registry.Register<int>({}, "Core.Int"); });
    NTest::ExpectError(EError::INVALID_ARGUMENT, [&] { registry.Register<int>({1}, ""); });
    NTest::ExpectError(EError::INVALID_ARGUMENT, [&] { registry.Register<int>({1}, std::string{"I\0nt", 4}); });
    NTest::ExpectError(EError::INVALID_ARGUMENT, [&] { registry.Register<int>({1}, "Core.Int", 0); });
    NTest::ExpectError(EError::INVALID_ARGUMENT, [&] { registry.Register<int>({1}, "Core.Int", 1, ETypeKind::ENUM); });
    NTest::ExpectError(EError::INVALID_ARGUMENT,
                       [&] { registry.Register<EMode>({1}, "Game.Mode", 1, ETypeKind::VALUE); });
    NTest::ExpectError(EError::INVALID_ARGUMENT,
                       [&] { registry.Register<int>({1}, "Core.Int", 1, static_cast<ETypeKind>(42)); });
    registry.Register<int>({1}, "Core.Int");
    registry.Freeze();
    EXPECT_EQ(registry.GetTypes().size(), 1);
}

TEST(TypeRegistry, FrozenRegistryProvidesExplicitMissingDescriptorAndNoInferredSerialization) {
    TypeRegistry registry;
    RegisterGameplay(registry);
    RegisterAssets(registry);
    NTest::ExpectError(EError::INVALID_STATE, [&] { (void)registry.Find<Position>(); });
    NTest::ExpectError(EError::INVALID_STATE, [&] { (void)registry.GetTypes(); });
    NTest::ExpectError(EError::INVALID_STATE, [&] { (void)registry.Create(TypeId{1002}); });
    registry.Freeze();
    registry.Freeze();
    NTest::ExpectError(EError::INVALID_STATE, [&] { registry.Register<float>({9}, "Core.Float"); });
    EXPECT_EQ(registry.Find<int>(), nullptr);
    EXPECT_EQ(registry.Find(TypeId{}), nullptr);
    EXPECT_EQ(registry.Find("Unknown"), nullptr);
    NTest::ExpectError(EError::NOT_FOUND, [&] { (void)registry.Get<int>(); });
    NTest::ExpectError(EError::NOT_FOUND, [&] { (void)registry.Create(TypeId{999}); });
    NTest::ExpectError(EError::NOT_FOUND, [&] { (void)registry.Make<int>(4); });
    for (const auto& descriptor: registry.GetTypes()) {
        EXPECT_FALSE(descriptor->GetCapabilities().Serializable);
    }
    const auto mode = registry.Get<EMode>();
    EXPECT_EQ(mode->GetKind(), ETypeKind::ENUM);
    ASSERT_TRUE(mode->GetEnumInfo().has_value());
    EXPECT_EQ(mode->GetEnumInfo()->UnderlyingSize, sizeof(std::int16_t));
    EXPECT_TRUE(mode->GetEnumInfo()->IsSigned);
    auto value = registry.Make<EMode>(EMode::ACTIVE);
    EXPECT_EQ(value.Get<EMode>(), EMode::ACTIVE);
}

struct Counters {
    int Alive = 0;
    int Copies = 0;
    int Moves = 0;
    int Destroyed = 0;
};

struct alignas(128) Tracked {
    explicit Tracked(Counters& counters, std::string text)
        : Counts(counters)
        , Text(std::move(text)) {
        ++Counts.Alive;
    }
    Tracked(const Tracked& other)
        : Counts(other.Counts)
        , Text(other.Text) {
        ++Counts.Copies;
        ++Counts.Alive;
    }
    Tracked(Tracked&& other) noexcept
        : Counts(other.Counts)
        , Text(std::move(other.Text)) {
        ++Counts.Moves;
        ++Counts.Alive;
    }
    ~Tracked() {
        --Counts.Alive;
        ++Counts.Destroyed;
    }
    Counters& Counts;
    std::string Text;
};

TEST(TypeRegistry, NontrivialAlignedValuesUseTypedCreateCopyMoveAndExactlyOnceDestroy) {
    Counters counters;
    TypeRegistry registry;
    registry.Register<Tracked>({1}, "Game.Tracked");
    registry.Register<std::string>({2}, "Core.String");
    registry.Freeze();
    const auto metadata = registry.Get<Tracked>();
    EXPECT_EQ(metadata->GetAlignment(), 128);
    EXPECT_FALSE(metadata->GetCapabilities().DefaultConstructible);
    EXPECT_TRUE(metadata->GetCapabilities().CopyConstructible);
    EXPECT_TRUE(metadata->GetCapabilities().MoveConstructible);
    NTest::ExpectError(EError::UNSUPPORTED, [&] { (void)registry.Create(TypeId{1}); });
    {
        auto first = registry.Make<Tracked>(counters, "retained payload");
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(first.TryGet<Tracked>()) % alignof(Tracked), 0);
        EXPECT_EQ(first.GetType().GetId(), TypeId{1});
        EXPECT_EQ(first.TryGet<std::string>(), nullptr);
        NTest::ExpectError(EError::INVALID_ARGUMENT, [&] { (void)first.Get<std::string>(); });
        const auto copy = first.Copy();
        EXPECT_EQ(copy.Get<Tracked>().Text, "retained payload");
        auto moved = first.MoveValue();
        EXPECT_FALSE(first.IsValid());
        EXPECT_EQ(moved.Get<Tracked>().Text, "retained payload");
        auto transferred = std::move(moved);
        EXPECT_FALSE(moved.IsValid()); // NOLINT(bugprone-use-after-move): verifies the documented empty source.
        EXPECT_EQ(counters.Moves, 1);
        EXPECT_EQ(counters.Copies, 1);
        auto assigned = registry.Create(TypeId{2});
        EXPECT_TRUE(assigned.Get<std::string>().empty());
        assigned = std::move(transferred);
        EXPECT_FALSE(transferred.IsValid()); // NOLINT(bugprone-use-after-move): verifies the documented empty source.
        assigned.Reset();
        assigned.Reset();
        EXPECT_EQ(counters.Alive, 1);
    }
    EXPECT_EQ(counters.Alive, 0);
    EXPECT_EQ(counters.Destroyed, 3);
}

struct Immovable {
    Immovable() = default;
    Immovable(const Immovable&) = delete;
    Immovable(Immovable&&) = delete;
};

TEST(TypeRegistry, MoveOnlyAndImmovableValuesRejectUnsupportedOperationsWithoutChangingOwnership) {
    TypeRegistry registry;
    registry.Register<std::unique_ptr<int>>({1}, "Core.UniqueInt");
    registry.Register<Immovable>({2}, "Game.Immovable");
    registry.Freeze();
    auto value = registry.Make<std::unique_ptr<int>>(std::make_unique<int>(17));
    EXPECT_FALSE(value.GetType().GetCapabilities().CopyConstructible);
    NTest::ExpectError(EError::UNSUPPORTED, [&] { (void)value.Copy(); });
    auto moved = value.MoveValue();
    EXPECT_FALSE(value.IsValid());
    EXPECT_EQ(*std::as_const(moved).Get<std::unique_ptr<int>>(), 17);
    auto immovable = registry.Create(TypeId{2});
    NTest::ExpectError(EError::UNSUPPORTED, [&] { (void)immovable.Copy(); });
    NTest::ExpectError(EError::UNSUPPORTED, [&] { (void)immovable.MoveValue(); });
    EXPECT_TRUE(immovable.IsValid());
    EXPECT_FALSE(immovable.GetType().GetCapabilities().MoveConstructible);
    auto transferred = std::move(immovable);
    EXPECT_TRUE(transferred.IsValid());
    EXPECT_FALSE(immovable.IsValid()); // NOLINT(bugprone-use-after-move): verifies the documented empty source.
}

struct Throwing {
    explicit Throwing(bool fail = true) {
        if (fail) {
            throw std::runtime_error{"constructor"};
        }
    }
    Throwing(const Throwing&) {
        throw std::runtime_error{"copy"};
    }
    Throwing(Throwing&&) { // NOLINT(performance-noexcept-move-constructor): intentional failure injection.
        throw std::runtime_error{"move"};
    }
};

TEST(TypeRegistry, ConstructorCopyAndMoveFailuresLeaveTheRegistryAndOriginalOwnerUsable) {
    TypeRegistry registry;
    registry.Register<Throwing>({1}, "Game.Throwing");
    registry.Freeze();
    EXPECT_THROW((void)registry.Create(TypeId{1}), std::runtime_error);
    EXPECT_THROW((void)registry.Make<Throwing>(true), std::runtime_error);
    auto original = registry.Make<Throwing>(false);
    EXPECT_THROW((void)original.Copy(), std::runtime_error);
    EXPECT_TRUE(original.IsValid());
    EXPECT_THROW((void)original.MoveValue(), std::runtime_error);
    EXPECT_TRUE(original.IsValid());
    EXPECT_NE(original.TryGet<Throwing>(), nullptr);
    EXPECT_EQ(registry.GetTypes().size(), 1);
}

TEST(TypeRegistry, ValuesAndMetadataOutliveRegistryWithoutHoldingWorldOrRegistryReferences) {
    TypeValue value;
    std::shared_ptr<const NCommon::TypeDescriptor> metadata;
    {
        TypeRegistry registry;
        registry.Register<std::string>({42}, "Core.String");
        registry.Freeze();
        metadata = registry.Get<std::string>();
        value = registry.Make<std::string>("lives independently");
    }
    EXPECT_EQ(metadata->GetName(), "Core.String");
    EXPECT_EQ(value.GetType().GetId(), metadata->GetId());
    auto copy = value.Copy();
    EXPECT_EQ(copy.Get<std::string>(), "lives independently");
    value.Reset();
    EXPECT_EQ(value.TryGet<std::string>(), nullptr);
    NTest::ExpectError(EError::INVALID_STATE, [&] { (void)value.GetType(); });
    NTest::ExpectError(EError::INVALID_STATE, [&] { (void)value.Copy(); });
    NTest::ExpectError(EError::INVALID_STATE, [&] { (void)value.MoveValue(); });
}

struct NullableAllocation {
    static inline bool Fail = false;
    static void* operator new(std::size_t size) noexcept {
        return Fail ? nullptr : ::operator new(size, std::nothrow);
    }
    static void operator delete(void* value) noexcept {
        ::operator delete(value);
    }
};

TEST(TypeRegistry, FailedClassAllocationCannotPublishAnEmptyValueOrDestroyTheMoveSource) {
    TypeRegistry registry;
    registry.Register<NullableAllocation>({1}, "Game.NullableAllocation");
    registry.Freeze();
    auto source = registry.Create(TypeId{1});
    NullableAllocation::Fail = true;
    EXPECT_THROW((void)registry.Create(TypeId{1}), std::bad_alloc);
    EXPECT_THROW((void)registry.Make<NullableAllocation>(), std::bad_alloc);
    EXPECT_THROW((void)source.Copy(), std::bad_alloc);
    EXPECT_THROW((void)source.MoveValue(), std::bad_alloc);
    EXPECT_TRUE(source.IsValid());
    NullableAllocation::Fail = false;
    auto moved = source.MoveValue();
    EXPECT_TRUE(moved.IsValid());
    EXPECT_FALSE(source.IsValid());
}

TEST(TypeRegistry, ConcurrentReadersObserveFrozenPublicationAndForeignConfigurationIsRejected) {
    TypeRegistry registry;
    std::barrier start{9};
    std::atomic<int> completed{0};
    std::array<std::thread, 8> readers;
    for (auto& reader: readers) {
        reader = std::thread{[&] {
            NTest::ExpectError(EError::INVALID_STATE, [&] { registry.Register<int>({9}, "Core.Int"); });
            NTest::ExpectError(EError::INVALID_STATE, [&] { registry.Freeze(); });
            start.arrive_and_wait();
            while (!registry.IsFrozen()) {
                std::this_thread::yield();
            }
            for (int iteration = 0; iteration < 1000; ++iteration) {
                const auto descriptor = registry.Get<Position>();
                if (descriptor->GetId() == TypeId{1002} && registry.GetTypes().size() == 3 &&
                    registry.Create(TypeId{1002}).Get<Position>().X == 0) {
                    ++completed;
                }
            }
        }};
    }
    start.arrive_and_wait();
    RegisterAssets(registry);
    RegisterGameplay(registry);
    registry.Freeze();
    for (auto& reader: readers) {
        reader.join();
    }
    EXPECT_EQ(completed.load(), 8000);
}

TEST(TypeRegistry, UnregisteredOrdinaryCppTypesNeedNoMetadataAndEmptyRegistryIsValid) {
    Position ordinary{.X = 7};
    EXPECT_EQ(ordinary.X, 7);
    TypeRegistry registry;
    registry.Freeze();
    EXPECT_TRUE(registry.GetTypes().empty());
    EXPECT_EQ(registry.Find<Position>(), nullptr);
    TypeValue empty;
    EXPECT_FALSE(empty.IsValid());
    empty.Reset();
    NTest::ExpectError(EError::INVALID_ARGUMENT, [&] { (void)empty.Get<Position>(); });
}

} // namespace
