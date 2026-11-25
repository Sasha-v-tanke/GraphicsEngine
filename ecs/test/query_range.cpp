#include <array>
#include <future>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include <ecs/internal/scoped_batch.h>
#include <ecs/world.h>
#include <gtest/gtest.h>
#include <lib/thread/task/task_system.h>
#include <tests/common/test_error.h>

namespace {
struct Value {
    int Number = 0;
};
struct Tag {};
struct Missing {};
using Query = decltype(std::declval<NEcs::World&>().Query<Value>());
using Iterator = Query::Iterator;
static_assert(std::ranges::input_range<Query>);
using NEcs::NInternal::BatchScope;
using NEcs::NInternal::ScopedBatchContext;
using NEcs::NInternal::ScopedRead;
using NEcs::NInternal::ScopedWorldAccess;
using NEcs::NInternal::ScopedWrite;

NEcs::EntityRef Add(NEcs::World& world, int value = 0) {
    return world.GetEntityRef(world.QueueCreateEntity()).AddComponent<Value>(value).AddComponent<Tag>();
}

TEST(QueryRange, RangeAndForEachShareDisabledMembershipAndReferences) {
    NEcs::World world;
    std::vector<NEcs::EntityRef> entities;
    entities.reserve(8);
    for (int mask = 0; mask < 8; ++mask) {
        entities.push_back(Add(world, mask));
    }
    world.CommitPending();
    for (int mask = 0; mask < 8; ++mask) {
        auto& entity = entities[static_cast<std::size_t>(mask)];
        if (mask & 1) {
            entity.Disable();
        }
        if (mask & 2) {
            entity.DisableComponent<Value>();
        }
        if (mask & 4) {
            entity.DisableComponent<Tag>();
        }
    }
    world.CommitPending();
    std::vector<int> ranged, callbacks, legacy;
    for (auto [entity, value, tag]: world.Query<Value, Tag>()) {
        static_assert(std::is_same_v<decltype(entity), NEcs::EntityRef>);
        static_assert(std::is_same_v<decltype(value), Value&>);
        EXPECT_EQ(&entity.GetComponent<Value>(), &value);
        EXPECT_EQ(&entity.GetComponent<Tag>(), &tag);
        ranged.push_back(value.Number);
    }
    world.ForEach<Value, Tag>([&](const NEcs::EntityRef&, Value& value, Tag&) { callbacks.push_back(value.Number); });
    world.Query<Value, Tag>([&](NEcs::Entity, Value& value, Tag&) { legacy.push_back(value.Number); });
    EXPECT_EQ(ranged, (std::vector<int>{0}));
    EXPECT_EQ(ranged, callbacks);
    EXPECT_EQ(ranged, legacy);
    ranged.clear();
    callbacks.clear();
    for (auto [entity, value, tag]: world.Query<Value, Tag>().IncludeDisabled()) {
        static_cast<void>(entity);
        static_cast<void>(tag);
        ranged.push_back(value.Number);
    }
    world.Query<Value, Tag>().IncludeDisabled().ForEach(
            [&](const NEcs::EntityRef&, Value& value, Tag&) { callbacks.push_back(value.Number); });
    EXPECT_EQ(ranged, (std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7}));
    EXPECT_EQ(ranged, callbacks);
}

TEST(QueryRange, StructuredBindingsBreakContinueEmptyAndMoveOnlyValues) {
    NEcs::World world;
    for (int value = 0; value < 10; ++value) {
        Add(world, value);
    }
    world.CommitPending();
    int sum = 0;
    for (auto [entity, value]: world.Query<Value>()) {
        static_cast<void>(entity);
        if (value.Number % 2) {
            continue;
        }
        if (value.Number == 6) {
            break;
        }
        sum += value.Number;
        value.Number += 100;
    }
    EXPECT_EQ(sum, 6);
    EXPECT_TRUE(world.Query<Missing>().begin() == std::default_sentinel);
    struct Owned {
        std::unique_ptr<int> Pointer;
    };
    auto entity = world.GetEntityRef(world.QueueCreateEntity()).AddComponent<Owned>(std::make_unique<int>(7));
    world.CommitPending();
    for (auto [reference, owned]: world.Query<Owned>()) {
        EXPECT_EQ(reference.GetReference(), entity.GetReference());
        *owned.Pointer = 19;
    }
    EXPECT_EQ(*entity.GetComponent<Owned>().Pointer, 19);
}

TEST(QueryRange, ConstWorldAndExplicitReadCannotExposeMutableComponentReferences) {
    NEcs::World world;
    Add(world, 8);
    world.CommitPending();
    std::as_const(world).ForEach<Value>([](NEcs::EntityRef entity, const Value& value) {
        EXPECT_EQ(value.Number, 8);
        EXPECT_EQ(std::as_const(entity).GetComponent<Value>().Number, 8);
        NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { entity.GetComponent<Value>().Number = 9; });
        NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { entity.Disable(); });
    });
    for (auto [entity, value]: world.Query<const Value>()) {
        static_cast<void>(entity);
        static_assert(std::is_same_v<decltype(value), const Value&>);
        EXPECT_EQ(value.Number, 8);
    }
}

TEST(QueryRange, ActiveRangesFreezeMembershipAndBlockCommitMutationAndRun) {
    NEcs::World world;
    auto entity = Add(world, 7);
    world.CommitPending();
    Iterator escaped;
    {
        auto query = world.Query<Value>();
        auto iterator = query.begin();
        escaped = iterator;
        entity.Disable();
        Add(world, 9);
        EXPECT_EQ(std::get<1>(*iterator).Number, 7);
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { world.CommitPending(); });
        NTest::ExpectError(NCommon::EError::INVALID_STATE,
                           [&] { world.RemoveComponent<Value>(entity.GetReference().Identity); });
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { world.CreateEntity(); });
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { world.RunSystems(); });
        auto foreign = std::async(std::launch::async, [&] {
            NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(*iterator); });
            NTest::ExpectError(NCommon::EError::INVALID_STATE,
                               [&] { world.DestroyEntity(entity.GetReference().Identity); });
        });
        foreign.get();
        ++iterator;
        EXPECT_TRUE(iterator == query.end());
    }
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(*escaped); });
    world.CommitPending();
    unsigned count = 0;
    world.ForEach<Value>([&](const auto&, auto&) { ++count; });
    EXPECT_EQ(count, 1U);
}

TEST(QueryRange, UnstartedSelectionsRejectCommitPhaseAndWorldBoundaries) {
    NEcs::World world;
    auto before = world.Query<Value>();
    Add(world);
    world.CommitPending();
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(before.begin()); });
    auto beforeRun = world.Query<Value>();
    world.RunSystems();
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(beforeRun.begin()); });
    std::optional<Query> expired;
    Iterator iterator;
    {
        NEcs::World temporary;
        Add(temporary);
        temporary.CommitPending();
        expired = temporary.Query<Value>();
        iterator = expired->begin();
    }
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(expired->begin()); });
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(*iterator); });
    expired.reset();
}

TEST(QueryRange, CallbackFailureRetiresOwnerReaderAndEmptyQueriesStillCheckAccess) {
    NEcs::World world;
    Add(world);
    world.CommitPending();
    EXPECT_THROW(world.ForEach<Value>([](const NEcs::EntityRef&, Value&) { throw std::runtime_error("callback"); }),
                 std::runtime_error);
    world.CommitPending();
    world.RegisterSystem({NEcs::Read<Missing>()}, [](NEcs::World& current) {
        NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { static_cast<void>(current.Query<Missing>()); });
        NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { static_cast<void>(current.Query<const Value>()); });
        auto empty = current.Query<const Missing>();
        EXPECT_TRUE(empty.begin() == empty.end());
    });
    world.RunSystems();
}

TEST(QueryRange, QueryAndRunAdmissionRejectAlreadyActiveStructuralWriter) {
    NEcs::World world;
    auto identity = world.CreateEntity();
    std::promise<void> entered;
    std::promise<void> release;
    auto ready = entered.get_future();
    const auto finish = release.get_future().share();
    struct Blocking {
        Blocking(std::promise<void>& signal, const std::shared_future<void>& done) {
            signal.set_value();
            done.wait();
        }
    };
    auto mutation = std::async(std::launch::async, [&] { world.AddComponent<Blocking>(identity, entered, finish); });
    ready.wait();
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(world.Query<Blocking>().begin()); });
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { world.RunSystems(); });
    NCommon::TaskSystem tasks{1};
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { world.RunSystems(tasks); });
    release.set_value();
    mutation.get();
    world.RunSystems();
    unsigned count = 0;
    world.ForEach<Blocking>([&](const auto&, Blocking&) { ++count; });
    EXPECT_EQ(count, 1U);
}

TEST(QueryRange, RegisteredReadWriteScopesAndEscapedReadersRetireOnSuccessAndFailure) {
    for (unsigned workers: {0U, 1U, 2U, 8U}) {
        for (bool fail: {false, true}) {
            NEcs::World world;
            auto entity = Add(world, 1);
            world.CommitPending();
            std::optional<Query> escaped;
            Iterator iterator;
            world.RegisterSystem(
                    {NEcs::Write<Value>()},
                    [&](NEcs::World& current) {
                        escaped = current.Query<Value>();
                        iterator = escaped->begin();
                        auto [reference, value] = *iterator;
                        value.Number += 1;
                        reference.Disable();
                        if (fail) {
                            throw std::runtime_error("range failure");
                        }
                    },
                    NEcs::SystemSchedule{.Id = 1},
                    1);
            auto run = [&] {
                if (workers == 0) {
                    world.RunSystems();
                } else {
                    NCommon::TaskSystem tasks{workers};
                    world.RunSystems(tasks);
                }
            };
            if (fail) {
                EXPECT_THROW(run(), std::runtime_error);
            } else {
                run();
            }
            EXPECT_EQ(entity.GetComponent<Value>().Number, 2);
            EXPECT_EQ(entity.IsEnabled(), fail);
            NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(*iterator); });
            NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { escaped->ForEach([](const auto&, auto&) {}); });
            // Escaped ranges must not obstruct later commit or shutdown.
            world.CommitPending();
        }
    }
}

TEST(QueryRange, ScopedIntersectionChecksDeclarationsAndPreservesDisjointWriters) {
    for (unsigned workers: {1U, 2U, 8U}) {
        NEcs::World world;
        std::vector<NEcs::EntityRef> entities;
        entities.reserve(8);
        for (int i = 0; i < 8; ++i) {
            entities.push_back(Add(world, i));
        }
        world.CommitPending();
        for (std::size_t i = 0; i < entities.size(); ++i) {
            ScopedWorldAccess::Register(
                    world,
                    {ScopedWrite<Value>(BatchScope::Entity(entities[i].GetReference())),
                     ScopedRead<Tag>(BatchScope::Entity(entities[i].GetReference()))},
                    [&, i](ScopedBatchContext& context) {
                        NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT,
                                           [&] { static_cast<void>(context.Query<Value, Tag>()); });
                        NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT,
                                           [&] { static_cast<void>(context.Query<Missing>()); });
                        unsigned count = 0;
                        for (auto [entity, value, tag]: context.Query<Value, const Tag>()) {
                            static_cast<void>(tag);
                            static_assert(std::is_same_v<decltype(value), Value&>);
                            EXPECT_EQ(entity.GetReference(), entities[i].GetReference());
                            value.Number += 10;
                            ++count;
                        }
                        EXPECT_EQ(count, 1U);
                        context.ForEach<const Value, const Tag>([](const auto&, const Value&, const Tag&) {});
                    },
                    NEcs::SystemSchedule{.Id = i + 1});
        }
        NCommon::TaskSystem tasks{workers};
        world.RunSystems(tasks);
        for (std::size_t i = 0; i < entities.size(); ++i) {
            EXPECT_EQ(entities[i].GetComponent<Value>().Number, static_cast<int>(i) + 10);
        }
    }
}
} // namespace
