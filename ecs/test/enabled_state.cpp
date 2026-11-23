#include <array>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include <ecs/entity_ref.h>
#include <ecs/internal/scoped_batch.h>
#include <gtest/gtest.h>
#include <lib/thread/task/task_system.h>
#include <tests/common/test_error.h>

namespace {
struct Value {
    int Number = 7;
};
struct Tag {};
struct Missing {};
using NEcs::NInternal::BatchScope;
using NEcs::NInternal::ScopedBatchContext;
using NEcs::NInternal::ScopedWorldAccess;
using NEcs::NInternal::ScopedWrite;

TEST(EnabledState, FullEntityAndRequiredComponentMatrixPreservesGetAndHas) {
    for (unsigned mask = 0; mask < 8; ++mask) {
        NEcs::World world;
        auto entity = world.GetEntityRef(world.QueueCreateEntity()).AddComponent<Value>(19).AddComponent<Tag>();
        world.CommitPending();
        if (mask & 1U) {
            entity.Disable();
        }
        if (mask & 2U) {
            entity.DisableComponent<Value>();
        }
        if (mask & 4U) {
            entity.DisableComponent<Tag>();
        }
        EXPECT_TRUE(entity.IsEnabled());
        EXPECT_TRUE(entity.IsComponentEnabled<Value>());
        EXPECT_TRUE(entity.IsComponentEnabled<Tag>());
        world.CommitPending();
        EXPECT_EQ(entity.IsEnabled(), !(mask & 1U));
        EXPECT_EQ(entity.IsComponentEnabled<Value>(), !(mask & 2U));
        EXPECT_EQ(entity.IsComponentEnabled<Tag>(), !(mask & 4U));
        EXPECT_TRUE(entity.HasComponent<Value>());
        EXPECT_TRUE(entity.HasComponent<Tag>());
        EXPECT_EQ(entity.GetComponent<Value>().Number, 19);
        EXPECT_EQ(std::as_const(entity).TryGetComponent<Value>(), &entity.GetComponent<Value>());
        unsigned legacy = 0, explicitSkip = 0, included = 0, single = 0, constIncluded = 0;
        world.Query<Value, Tag>([&](auto, auto&, auto&) { ++legacy; });
        world.Query<Value, Tag>().SkipDisabled().ForEach([&](auto, auto&, auto&) { ++explicitSkip; });
        world.Query<Value, Tag>().IncludeDisabled().ForEach([&](auto, auto&, auto&) { ++included; });
        world.Query<Value>([&](auto, auto&) { ++single; });
        std::as_const(world).Query<Value, Tag>().IncludeDisabled().ForEach(
                [&](auto, const Value&, const Tag&) { ++constIncluded; });
        EXPECT_EQ(legacy, mask == 0 ? 1U : 0U);
        EXPECT_EQ(explicitSkip, legacy);
        EXPECT_EQ(included, 1U);
        EXPECT_EQ(constIncluded, 1U);
        EXPECT_EQ(single, (mask & 3U) == 0 ? 1U : 0U);
        unsigned reset = 0;
        world.Query<Value, Tag>().IncludeDisabled().SkipDisabled().ForEach([&](auto, auto&, auto&) { ++reset; });
        EXPECT_EQ(reset, legacy);
    }
}

TEST(EnabledState, PendingTogglesAndIdempotencePublishOnlyAtCommit) {
    NEcs::World world;
    auto entity = world.GetEntityRef(world.QueueCreateEntity()).AddComponent<Value>();
    entity.Disable();
    entity.Disable();
    entity.Enable();
    entity.Disable();
    entity.DisableComponent<Value>();
    entity.EnableComponent<Value>();
    entity.DisableComponent<Value>();
    EXPECT_FALSE(entity.IsEnabled());
    NTest::ExpectError(NCommon::EError::NOT_READY, [&] { (void)entity.IsComponentEnabled<Value>(); });
    world.CommitPending();
    EXPECT_FALSE(entity.IsEnabled());
    EXPECT_FALSE(entity.IsComponentEnabled<Value>());
    entity.Enable();
    entity.Enable();
    entity.EnableComponent<Value>();
    entity.EnableComponent<Value>();
    EXPECT_FALSE(entity.IsEnabled());
    EXPECT_FALSE(entity.IsComponentEnabled<Value>());
    world.CommitPending();
    EXPECT_TRUE(entity.IsEnabled());
    EXPECT_TRUE(entity.IsComponentEnabled<Value>());
    entity.DisableComponent<Value>();
    world.CommitPending();
    entity.GetComponent<Value>().Number = 31;
    entity.EnableComponent<Value>();
    world.CommitPending();
    EXPECT_EQ(entity.GetComponent<Value>().Number, 31);
}

TEST(EnabledState, QueuedAddCanBeDisabledOnCommittedEntityAndMissingComponentsFail) {
    NEcs::World world;
    auto entity = world.GetEntityRef(world.QueueCreateEntity());
    world.CommitPending();
    NTest::ExpectError(NCommon::EError::NOT_FOUND, [&] { entity.EnableComponent<Missing>(); });
    NTest::ExpectError(NCommon::EError::NOT_FOUND, [&] { entity.DisableComponent<Missing>(); });
    NTest::ExpectError(NCommon::EError::NOT_FOUND, [&] { (void)entity.IsComponentEnabled<Missing>(); });
    entity.AddComponent<Value>(29);
    entity.DisableComponent<Value>();
    EXPECT_FALSE(entity.HasComponent<Value>());
    world.CommitPending();
    EXPECT_FALSE(entity.IsComponentEnabled<Value>());
    EXPECT_EQ(entity.GetComponent<Value>().Number, 29);
    entity.RemoveComponent<Value>();
    NTest::ExpectError(NCommon::EError::NOT_FOUND, [&] { entity.EnableComponent<Value>(); });
    world.CommitPending();
    entity.AddComponent<Value>(44);
    world.CommitPending();
    EXPECT_TRUE(entity.IsComponentEnabled<Value>());
}

TEST(EnabledState, EntityDisableDoesNotCascadeAndRecycledSlotsStartEnabled) {
    NEcs::World world;
    auto parent = world.GetEntityRef(world.QueueCreateEntity()).AddComponent<Value>();
    auto child = world.GetEntityRef(world.QueueCreateEntity()).AddComponent<Value>();
    world.QueueReparent(child.GetReference(), parent.GetReference());
    parent.Disable();
    parent.DisableComponent<Value>();
    world.CommitPending();
    EXPECT_FALSE(parent.IsEnabled());
    EXPECT_TRUE(child.IsEnabled());
    unsigned count = 0;
    world.Query<Value>([&](auto entity, auto&) {
        EXPECT_EQ(entity, child.GetReference().Identity);
        ++count;
    });
    EXPECT_EQ(count, 1U);
    const auto retired = parent.GetReference();
    parent.Destroy();
    world.CommitPending();
    auto replacement = world.GetEntityRef(world.QueueCreateEntity()).AddComponent<Value>();
    world.CommitPending();
    EXPECT_EQ(replacement.GetReference().Identity.Index, retired.Identity.Index);
    EXPECT_TRUE(replacement.IsEnabled());
    EXPECT_TRUE(replacement.IsComponentEnabled<Value>());
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { parent.Disable(); });
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)parent.IsEnabled(); });
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { parent.EnableComponent<Value>(); });
}

TEST(EnabledState, EmptyAndExpiredHandlesAndQueriesRejectAccess) {
    NEcs::EntityRef expired;
    using Selection = decltype(std::declval<NEcs::World&>().Query<Value>());
    std::optional<Selection> query;
    {
        NEcs::World world;
        query = world.Query<Value>();
        query->IncludeDisabled().ForEach([](auto, auto&) { FAIL() << "Empty World"; });
        expired = world.GetEntityRef(world.QueueCreateEntity()).AddComponent<Value>();
        world.CommitPending();
    }
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { expired.Enable(); });
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)expired.IsEnabled(); });
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)expired.IsComponentEnabled<Value>(); });
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { query->ForEach([](auto, auto&) {}); });
    NEcs::EntityRef empty;
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { empty.Disable(); });
}

TEST(EnabledState, QueuedTogglesDoNotChangeCurrentQueryMembership) {
    NEcs::World world;
    std::vector<NEcs::EntityRef> entities;
    entities.reserve(64);
    for (int index = 0; index < 64; ++index) {
        entities.push_back(world.GetEntityRef(world.QueueCreateEntity()).AddComponent<Value>(index));
    }
    world.CommitPending();
    unsigned seen = 0;
    world.Query<Value>([&](auto entity, auto&) {
        world.GetEntityRef(world.GetEntityReference(entity)).Disable();
        ++seen;
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { world.CommitPending(); });
    });
    EXPECT_EQ(seen, 64U);
    seen = 0;
    world.Query<Value>([&](auto, auto&) { ++seen; });
    EXPECT_EQ(seen, 64U);
    world.CommitPending();
    world.Query<Value>([](auto, auto&) { FAIL() << "Disabled entity reached default Query"; });
}

TEST(EnabledState, FailedSystemDiscardsEnabledChangesAndEscapedSelectionExpires) {
    for (unsigned workers: {0U, 1U, 2U, 8U}) {
        NEcs::World world;
        auto entity = world.GetEntityRef(world.QueueCreateEntity()).AddComponent<Value>();
        world.CommitPending();
        using Selection = decltype(world.Query<Value>());
        std::optional<Selection> escaped;
        world.RegisterSystem(
                {NEcs::Read<Value>()},
                [&](NEcs::World& current) {
                    escaped = current.Query<Value>();
                    EXPECT_TRUE(entity.IsEnabled());
                    entity.Disable();
                    entity.DisableComponent<Value>();
                    EXPECT_TRUE(entity.IsComponentEnabled<Value>());
                    throw std::runtime_error("discard queued enabled changes");
                },
                NEcs::SystemSchedule{.Id = 1},
                1);
        if (workers == 0) {
            EXPECT_THROW(world.RunSystems(), std::runtime_error);
        } else {
            NCommon::TaskSystem tasks{workers};
            EXPECT_THROW(world.RunSystems(tasks), std::runtime_error);
        }
        EXPECT_TRUE(entity.IsEnabled());
        EXPECT_TRUE(entity.IsComponentEnabled<Value>());
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { escaped->ForEach([](auto, auto&) {}); });
    }
}

TEST(EnabledState, DetachedWorkersCannotUseFacadeOrPolicySelection) {
    NEcs::World world;
    auto entity = world.GetEntityRef(world.QueueCreateEntity()).AddComponent<Value>();
    world.CommitPending();
    const auto query = world.Query<Value>();
    NCommon::TaskSystem tasks{1};
    const auto task = tasks.Submit([&](NCommon::TaskContext&) {
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { entity.Disable(); });
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)entity.IsEnabled(); });
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { query.ForEach([](auto, auto&) {}); });
    });
    tasks.Wait(task);
    EXPECT_EQ(tasks.GetStatus(task), NCommon::ETaskStatus::COMPLETED);
}

TEST(EnabledState, ForeignHostThreadCannotUseEnabledFacadeOrQuerySelection) {
    NEcs::World world;
    auto entity = world.GetEntityRef(world.QueueCreateEntity()).AddComponent<Value>();
    world.CommitPending();
    const auto query = world.Query<Value>();
    const auto readQuery = std::as_const(world).Query<Value>();
    auto foreign = std::async(std::launch::async, [&] {
        EXPECT_FALSE(NCommon::TaskSystem::IsCurrentThreadWorker());
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { entity.Enable(); });
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { entity.Disable(); });
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { entity.EnableComponent<Value>(); });
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { entity.DisableComponent<Value>(); });
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)entity.IsEnabled(); });
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)entity.IsComponentEnabled<Value>(); });
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)world.Query<Value>(); });
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)std::as_const(world).Query<Value>(); });
        NTest::ExpectError(NCommon::EError::INVALID_STATE,
                           [&] { query.IncludeDisabled().ForEach([](auto, Value& value) { value.Number = 991; }); });
        NTest::ExpectError(NCommon::EError::INVALID_STATE,
                           [&] { readQuery.ForEach([](auto, const Value&) { FAIL() << "Foreign query executed"; }); });
    });
    foreign.get();
    world.CommitPending();
    EXPECT_TRUE(entity.IsEnabled());
    EXPECT_TRUE(entity.IsComponentEnabled<Value>());
    EXPECT_EQ(entity.GetComponent<Value>().Number, 7);
}

TEST(EnabledState, RegisteredInvocationsRetainQueryAndEnabledCapabilities) {
    for (unsigned workers: {0U, 1U, 2U, 8U}) {
        NEcs::World world;
        auto entity = world.GetEntityRef(world.QueueCreateEntity()).AddComponent<Value>();
        entity.Disable();
        world.CommitPending();
        unsigned visited = 0;
        world.RegisterSystem(
                {NEcs::Write<Value>()},
                [&](NEcs::World& current) {
                    current.Query<Value>().IncludeDisabled().ForEach([&](NEcs::Entity identity, Value& value) {
                        auto local = current.GetEntityRef(current.GetEntityReference(identity));
                        EXPECT_FALSE(local.IsEnabled());
                        EXPECT_TRUE(local.IsComponentEnabled<Value>());
                        local.Enable();
                        local.DisableComponent<Value>();
                        EXPECT_FALSE(local.IsEnabled());
                        EXPECT_TRUE(local.IsComponentEnabled<Value>());
                        ++value.Number;
                        ++visited;
                    });
                },
                NEcs::SystemSchedule{.Id = 1},
                1);
        if (workers == 0) {
            world.RunSystems();
        } else {
            NCommon::TaskSystem tasks{workers};
            world.RunSystems(tasks);
        }
        EXPECT_EQ(visited, 1U);
        EXPECT_TRUE(entity.IsEnabled());
        EXPECT_FALSE(entity.IsComponentEnabled<Value>());
        EXPECT_EQ(entity.GetComponent<Value>().Number, 8);
    }
}

TEST(EnabledState, ScopedForEachUsesFrozenEntityAndComponentFlagsAndGetStillWorks) {
    for (unsigned workers: {0U, 1U, 2U, 8U}) {
        NEcs::World world;
        std::vector<NEcs::EntityRef> entities;
        for (unsigned mask = 0; mask < 4; ++mask) {
            auto entity = world.GetEntityRef(world.QueueCreateEntity()).AddComponent<Value>(static_cast<int>(mask));
            if (mask & 1U) {
                entity.Disable();
            }
            if (mask & 2U) {
                entity.DisableComponent<Value>();
            }
            entities.push_back(entity);
        }
        world.CommitPending();
        ScopedWorldAccess::Register(
                world,
                {ScopedWrite<Value>()},
                [&](ScopedBatchContext& context) {
                    auto view = context.Write<Value>();
                    unsigned skip = 0, included = 0, explicitSkip = 0;
                    view.ForEach([&](auto identity, Value&) {
                        ++skip;
                        EXPECT_EQ(identity, entities[0].GetReference());
                        world.GetEntityRef(identity).Disable();
                    });
                    view.SkipDisabled().ForEach([&](auto, Value&) { ++explicitSkip; });
                    view.IncludeDisabled().ForEach([&](auto identity, Value& value) {
                        ++included;
                        EXPECT_EQ(view.Get(identity).Number, value.Number);
                        EXPECT_EQ(world.GetEntityRef(identity).template IsComponentEnabled<Value>(),
                                  (value.Number & 2) == 0);
                    });
                    EXPECT_EQ(skip, 1U);
                    EXPECT_EQ(explicitSkip, 1U);
                    EXPECT_EQ(included, 4U);
                },
                NEcs::SystemSchedule{.Id = 1},
                1);
        if (workers == 0) {
            world.RunSystems();
        } else {
            NCommon::TaskSystem tasks{workers};
            world.RunSystems(tasks);
        }
        EXPECT_FALSE(entities[0].IsEnabled());
    }
}

TEST(EnabledState, ConcurrentProducersQueueTogglesWithoutChangingFrozenMetadata) {
    NEcs::World world;
    std::array<NEcs::EntityRef, 2> entities;
    for (auto& entity: entities) {
        entity = world.GetEntityRef(world.QueueCreateEntity()).AddComponent<Value>();
    }
    world.CommitPending();
    std::mutex mutex;
    std::condition_variable overlap;
    unsigned arrived = 0;
    for (std::size_t index = 0; index < entities.size(); ++index) {
        ScopedWorldAccess::Register(
                world,
                {ScopedWrite<Value>(BatchScope::Entity(entities[index].GetReference()))},
                [&, index](ScopedBatchContext& context) {
                    {
                        std::unique_lock lock{mutex};
                        ++arrived;
                        overlap.notify_all();
                        EXPECT_TRUE(overlap.wait_for(lock, std::chrono::seconds{5}, [&] { return arrived == 2; }));
                    }
                    for (unsigned iteration = 0; iteration < 128; ++iteration) {
                        auto entity = world.GetEntityRef(entities[index].GetReference());
                        entity.Disable();
                        entity.Enable();
                        entity.DisableComponent<Value>();
                        entity.EnableComponent<Value>();
                        EXPECT_TRUE(entity.IsEnabled());
                        EXPECT_TRUE(entity.IsComponentEnabled<Value>());
                    }
                    context.Write<Value>().ForEach(
                            [&](auto, auto&) { world.GetEntityRef(entities[index].GetReference()).Disable(); });
                },
                NEcs::SystemSchedule{.Id = index + 1},
                static_cast<std::uint32_t>(index + 1));
    }
    NCommon::TaskSystem tasks{2};
    world.RunSystems(tasks);
    for (const auto& entity: entities) {
        EXPECT_FALSE(entity.IsEnabled());
        EXPECT_TRUE(entity.IsComponentEnabled<Value>());
    }
}
} // namespace
