#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <GraphicsEngine/ecs/system_context.h>
#include <GraphicsEngine/lib/thread/task/task_system.h>
#include <GraphicsEngine/scene/scene.h>
#include <ecs/internal/system_run.h>
#include <gtest/gtest.h>
#include <tests/common/test_error.h>

namespace {

struct Position {
    double Value = 0.0;
};
struct Velocity {
    double Value = 0.0;
};
struct Hidden {};

using MovementAccess = NEcs::NSystem::Access<NEcs::NSystem::Write<Position>, NEcs::NSystem::Read<Velocity>>;
using MovementContext = NEcs::NSystem::SystemContext<MovementAccess>;
using Selection = decltype(std::declval<MovementContext&>().Query<Position, Velocity>());
using Row = std::ranges::range_value_t<Selection>;

template<typename TContext>
concept CanQueryHidden = requires(TContext& context) { context.template Query<Hidden>(); };

static_assert(!CanQueryHidden<MovementContext>);
static_assert(std::is_same_v<std::tuple_element_t<1, Row>, Position&>);
static_assert(std::is_same_v<std::tuple_element_t<2, Row>, const Velocity&>);
static_assert(std::is_same_v<decltype(std::declval<MovementContext&>().Get<Velocity>(NEcs::EntityReference{})),
                             const Velocity&>);
static_assert(
        std::is_same_v<decltype(std::declval<MovementContext&>().Get<Position>(NEcs::EntityReference{})), Position&>);

class MovementSystem {
public:
    using Access = MovementAccess;
    static constexpr auto PHASE = NEcs::NSystem::ESystemPhase::UPDATE;

    MovementSystem(std::optional<MovementContext>& saved,
                   std::optional<Selection>& query,
                   NEcs::EntityRef& entity,
                   bool fail)
        : m_saved(saved)
        , m_query(query)
        , m_entity(entity)
        , m_fail(fail) {
    }

    void Update(MovementContext& context) {
        EXPECT_DOUBLE_EQ(context.DeltaTime(), 0.25);
        m_saved = context;
        m_query = context.Query<Position, Velocity>();
        for (auto [entity, position, velocity]: *m_query) {
            EXPECT_EQ(&context.Get<Position>(entity.GetReference()), &position);
            EXPECT_EQ(&context.Get<Velocity>(entity.GetReference()), &velocity);
            NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT,
                               [&] { static_cast<void>(entity.GetComponent<Velocity>()); });
            NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT,
                               [&] { static_cast<void>(entity.HasComponent<Hidden>()); });
            position.Value += velocity.Value * context.DeltaTime();
            m_entity = context.GetEntity(entity.GetReference());
            entity.Disable();
            EXPECT_TRUE(entity.IsEnabled());
        }
        unsigned visited = 0;
        context.ForEach<Position, Velocity>([&](NEcs::EntityRef, Position& position, const Velocity& velocity) {
            EXPECT_DOUBLE_EQ(position.Value, 3.0);
            EXPECT_DOUBLE_EQ(velocity.Value, 8.0);
            ++visited;
        });
        EXPECT_EQ(visited, 1U);
        if (m_fail) {
            throw std::runtime_error{"typed system failure"};
        }
    }

private:
    std::optional<MovementContext>& m_saved;
    std::optional<Selection>& m_query;
    NEcs::EntityRef& m_entity;
    bool m_fail;
};

TEST(TypedSystems, AccessAndLifetimeMatchForSerialAndAsyncRuns) {
    for (unsigned workers: {0U, 1U, 2U, 8U}) {
        for (bool fail: {false, true}) {
            NScene::Scene scene;
            auto entity = scene.CreateEntity().AddComponent<Position>(1.0).AddComponent<Velocity>(8.0);
            std::optional<MovementContext> saved;
            std::optional<Selection> query;
            NEcs::EntityRef scoped;
            scene.AddSystem<MovementSystem>(saved, query, scoped, fail);
            auto run = [&] {
                if (workers == 0) {
                    scene.GetWorld().RunSystems(0.25);
                } else {
                    NCommon::TaskSystem tasks{workers};
                    scene.GetWorld().RunSystems(tasks, 0.25);
                }
            };
            if (fail) {
                EXPECT_THROW(run(), std::runtime_error);
            } else {
                EXPECT_NO_THROW(run());
            }
            EXPECT_DOUBLE_EQ(entity.GetComponent<Position>().Value, 3.0);
            EXPECT_EQ(entity.IsEnabled(), fail);
            ASSERT_TRUE(saved);
            ASSERT_TRUE(query);
            NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(saved->DeltaTime()); });
            NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(query->begin()); });
            NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(scoped.IsEnabled()); });
            EXPECT_NO_THROW(scene.CommitPending());
        }
    }
}

constexpr std::uint32_t LARGE_QUERY_COUNT = 16384;

struct LargeQuerySystem {
    using Access = MovementAccess;

    void Update(MovementContext& context) {
        std::uint32_t visited = 0;
        for (auto [entity, position, velocity]: context.Query<Position, Velocity>().IncludeDisabled()) {
            EXPECT_EQ(&context.Get<Position>(entity.GetReference()), &position);
            EXPECT_EQ(&context.Get<Velocity>(entity.GetReference()), &velocity);
            position.Value = velocity.Value;
            ++visited;
        }
        EXPECT_EQ(visited, LARGE_QUERY_COUNT / 2);
        visited = 0;
        context.ForEach<Position, Velocity>([&](NEcs::EntityRef, Position& position, const Velocity& velocity) {
            EXPECT_DOUBLE_EQ(position.Value, velocity.Value);
            ++visited;
        });
        EXPECT_EQ(visited, LARGE_QUERY_COUNT / 2 - 2);
    }
};

TEST(TypedSystems, LargeSparseQueryPreservesMembershipAcrossRuns) {
    NScene::Scene scene;
    auto& world = scene.GetWorld();
    for (std::uint32_t index = 0; index < LARGE_QUERY_COUNT; ++index) {
        const auto entity = world.CreateEntity();
        world.AddComponent<Position>(entity);
        if (index % 2 == 0) {
            world.AddComponent<Velocity>(entity, static_cast<double>(index + 1));
            if (index == 0) {
                world.GetEntityRef(world.GetEntityReference(entity)).Disable();
            } else if (index == 2) {
                world.GetEntityRef(world.GetEntityReference(entity)).DisableComponent<Velocity>();
            }
        }
    }
    world.CommitPending();
    scene.AddSystem<LargeQuerySystem>();
    world.RunSystems();
    NCommon::TaskSystem tasks{2};
    world.RunSystems(tasks);
    std::uint32_t visited = 0;
    for (auto [entity, position, velocity]: world.Query<const Position, const Velocity>().IncludeDisabled()) {
        EXPECT_DOUBLE_EQ(position.Value, velocity.Value);
        ++visited;
    }
    EXPECT_EQ(visited, LARGE_QUERY_COUNT / 2);
}

using WriteAccess = NEcs::NSystem::Access<NEcs::NSystem::Write<Position>>;
using WriteContext = NEcs::NSystem::SystemContext<WriteAccess>;

template<int Number>
struct Writer {
    using Access = WriteAccess;
    void Update(WriteContext& context) {
        context.ForEach<Position>(
                [](NEcs::EntityRef, Position& position) { position.Value = position.Value * 10.0 + Number; });
    }
};

TEST(TypedSystems, ConflictOrderDoesNotDependOnRegistrationOrWorkerCount) {
    std::optional<double> expected;
    for (unsigned workers: {0U, 1U, 2U, 8U}) {
        for (bool reverse: {false, true}) {
            NScene::Scene scene;
            auto entity = scene.CreateEntity().AddComponent<Position>();
            if (reverse) {
                scene.AddSystem<Writer<2>>();
                scene.AddSystem<Writer<1>>();
            } else {
                scene.AddSystem<Writer<1>>();
                scene.AddSystem<Writer<2>>();
            }
            if (workers == 0) {
                scene.GetWorld().RunSystems();
            } else {
                NCommon::TaskSystem tasks{workers};
                scene.GetWorld().RunSystems(tasks);
            }
            const double value = entity.GetComponent<Position>().Value;
            EXPECT_TRUE(value == 12.0 || value == 21.0);
            if (!expected) {
                expected = value;
            }
            EXPECT_DOUBLE_EQ(value, *expected);
        }
    }
}

struct OrderedWriter {
    using Access = WriteAccess;
    using After = NEcs::NSystem::After<Writer<1>>;
    void Update(WriteContext& context) {
        context.ForEach<Position>([](NEcs::EntityRef, Position& position) {
            EXPECT_DOUBLE_EQ(position.Value, 1.0);
            position.Value = 42.0;
        });
    }
};

struct CycleB;
struct CycleA {
    using Access = WriteAccess;
    using Before = NEcs::NSystem::Before<CycleB>;
    void Update(WriteContext&) {
        ADD_FAILURE();
    }
};
struct CycleB {
    using Access = WriteAccess;
    using Before = NEcs::NSystem::Before<CycleA>;
    void Update(WriteContext&) {
        ADD_FAILURE();
    }
};

TEST(TypedSystems, ExplicitDependenciesAndCyclesUseExistingGraph) {
    for (unsigned workers: {1U, 2U, 8U}) {
        NScene::Scene scene;
        auto entity = scene.CreateEntity().AddComponent<Position>();
        scene.AddSystem<OrderedWriter>();
        scene.AddSystem<Writer<1>>();
        NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { scene.AddSystem<Writer<1>>(); });
        NCommon::TaskSystem tasks{workers};
        scene.GetWorld().RunSystems(tasks);
        EXPECT_DOUBLE_EQ(entity.GetComponent<Position>().Value, 42.0);

        NScene::Scene cyclic;
        cyclic.AddSystem<CycleA>();
        cyclic.AddSystem<CycleB>();
        NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { cyclic.GetWorld().RunSystems(tasks); });
    }
}

TEST(TypedSystems, LegacyCallbacksCanCoexistAndPreparedCancelDoesNotInvokeSystems) {
    auto scene = std::make_shared<NScene::Scene>();
    auto entity = scene->CreateEntity().AddComponent<Position>();
    scene->GetWorld().RegisterSystem({}, [&](NEcs::World& world) {
        world.GetComponent<Position>(entity.GetReference().Identity).Value = 5.0;
    });
    scene->AddSystem<Writer<1>>();
    NCommon::TaskSystem tasks{2};
    scene->GetWorld().RunSystems(tasks);
    EXPECT_DOUBLE_EQ(entity.GetComponent<Position>().Value, 51.0);

    auto world = std::shared_ptr<NEcs::World>{scene, &scene->GetWorld()};
    std::promise<NEcs::NInternal::SystemRunOutcome> done;
    auto future = done.get_future();
    auto prepared = NEcs::NInternal::PrepareSystems(world, tasks, [&](const auto& result) { done.set_value(result); });
    prepared.GetHandle().RequestCancel();
    prepared.Start();
    ASSERT_EQ(future.wait_for(std::chrono::seconds{2}), std::future_status::ready);
    EXPECT_EQ(future.get().Status, NEcs::NInternal::ESystemRunStatus::CANCELLED);
    EXPECT_DOUBLE_EQ(entity.GetComponent<Position>().Value, 51.0);
}

} // namespace
