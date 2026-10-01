#include <vector>

#include <GraphicsEngine/ecs/world.h>
#include <gtest/gtest.h>
#include <tests/common/test_error.h>

namespace {

struct Position {
    int X = 0;
    int Y = 0;
};

struct Velocity {
    int X = 0;
    int Y = 0;
};

struct ConstructOnly {
    explicit ConstructOnly(int value)
        : Value(value) {
    }

    ConstructOnly(const ConstructOnly&) = delete;
    ConstructOnly(ConstructOnly&&) noexcept = default;

    ConstructOnly& operator=(const ConstructOnly&) = delete;
    ConstructOnly& operator=(ConstructOnly&&) = delete;

    int Value = 0;
};

} // namespace

TEST(EcsWorld, ReusesEntitySlotsWithNewGeneration) {
    NEcs::World world;

    const NEcs::Entity first = world.CreateEntity();
    ASSERT_TRUE(world.IsAlive(first));

    world.DestroyEntity(first);
    EXPECT_FALSE(world.IsAlive(first));

    const NEcs::Entity second = world.CreateEntity();

    EXPECT_EQ(second.Index, first.Index);
    EXPECT_NE(second.Generation, first.Generation);
    EXPECT_TRUE(world.IsAlive(second));
}

TEST(EcsWorld, RejectsStaleEntityComponentMutation) {
    NEcs::World world;

    const NEcs::Entity entity = world.CreateEntity();
    world.AddComponent<Position>(entity, 1, 2);
    world.DestroyEntity(entity);

    EXPECT_FALSE(world.HasComponent<Position>(entity));
    EXPECT_FALSE(world.RemoveComponent<Position>(entity));
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { world.AddComponent<Position>(entity, 3, 4); });
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT,
                       [&] { static_cast<void>(world.GetComponent<Position>(entity)); });
}

TEST(EcsWorld, AddsGetsReplacesAndRemovesComponents) {
    NEcs::World world;
    const NEcs::Entity entity = world.CreateEntity();

    auto& position = world.AddComponent<Position>(entity, 1, 2);
    EXPECT_EQ(position.X, 1);
    EXPECT_EQ(position.Y, 2);

    world.AddComponent<Position>(entity, 3, 4);
    EXPECT_EQ(world.GetComponent<Position>(entity).X, 3);
    EXPECT_TRUE(world.HasComponent<Position>(entity));
    EXPECT_TRUE(world.RemoveComponent<Position>(entity));
    EXPECT_FALSE(world.HasComponent<Position>(entity));
    EXPECT_FALSE(world.RemoveComponent<Position>(entity));
}

TEST(EcsWorld, SupportsConstructOnlyComponents) {
    NEcs::World world;
    const NEcs::Entity entity = world.CreateEntity();

    auto& first = world.AddComponent<ConstructOnly>(entity, 10);
    EXPECT_EQ(first.Value, 10);

    auto& replacement = world.AddComponent<ConstructOnly>(entity, 20);
    EXPECT_EQ(replacement.Value, 20);
    EXPECT_EQ(world.GetComponent<ConstructOnly>(entity).Value, 20);
}

TEST(EcsWorld, QueriesEntitiesWithAllRequestedComponents) {
    NEcs::World world;

    const NEcs::Entity moving = world.CreateEntity();
    world.AddComponent<Position>(moving, 10, 20);
    world.AddComponent<Velocity>(moving, 1, 2);

    const NEcs::Entity staticEntity = world.CreateEntity();
    world.AddComponent<Position>(staticEntity, 100, 200);

    std::vector<NEcs::Entity> visited;

    world.Query<Position, Velocity>([&](NEcs::Entity entity, Position& position, Velocity& velocity) {
        visited.push_back(entity);
        position.X += velocity.X;
        position.Y += velocity.Y;
    });

    ASSERT_EQ(visited.size(), 1);
    EXPECT_EQ(visited.front(), moving);
    EXPECT_EQ(world.GetComponent<Position>(moving).X, 11);
    EXPECT_EQ(world.GetComponent<Position>(moving).Y, 22);
    EXPECT_EQ(world.GetComponent<Position>(staticEntity).X, 100);
}

TEST(EcsWorld, QueryUsesStableEntityScope) {
    NEcs::World world;

    const NEcs::Entity first = world.CreateEntity();
    world.AddComponent<Position>(first, 1, 2);

    std::vector<NEcs::Entity> visited;

    world.Query<Position>([&](NEcs::Entity entity, Position&) {
        visited.push_back(entity);

        const NEcs::Entity created = world.CreateEntity();
        world.AddComponent<Position>(created, 3, 4);
    });

    ASSERT_EQ(visited.size(), 1);
    EXPECT_EQ(visited.front(), first);
    EXPECT_EQ(world.GetAliveEntityCount(), 2);
}

TEST(EcsWorld, QuerySkipsEntitiesReusedAfterSnapshot) {
    NEcs::World world;

    const NEcs::Entity first = world.CreateEntity();
    world.AddComponent<Position>(first, 1, 2);

    const NEcs::Entity second = world.CreateEntity();
    world.AddComponent<Position>(second, 3, 4);

    std::vector<NEcs::Entity> visited;

    world.Query<Position>([&](NEcs::Entity entity, Position&) {
        visited.push_back(entity);

        if (entity == first) {
            world.DestroyEntity(second);
            const NEcs::Entity reused = world.CreateEntity();
            ASSERT_EQ(reused.Index, second.Index);
            world.AddComponent<Position>(reused, 5, 6);
        }
    });

    ASSERT_EQ(visited.size(), 1);
    EXPECT_EQ(visited.front(), first);
}

TEST(EcsWorld, RemovesComponentsWhenEntityIsDestroyed) {
    NEcs::World world;

    const NEcs::Entity entity = world.CreateEntity();
    world.AddComponent<Position>(entity, 1, 2);
    world.AddComponent<Velocity>(entity, 3, 4);

    world.DestroyEntity(entity);

    const NEcs::Entity reused = world.CreateEntity();
    ASSERT_EQ(reused.Index, entity.Index);
    EXPECT_FALSE(world.HasComponent<Position>(reused));
    EXPECT_FALSE(world.HasComponent<Velocity>(reused));
}

TEST(EcsWorld, RegistersSequentialSystemsWithAccessDeclarations) {
    NEcs::World world;
    const NEcs::Entity entity = world.CreateEntity();
    world.AddComponent<Position>(entity, 1, 2);

    world.RegisterSystem({NEcs::Read<Velocity>(), NEcs::Write<Position>()}, [](NEcs::World& systemWorld) {
        systemWorld.Query<Position>([](NEcs::Entity, Position& position) { position.X += 5; });
    });

    ASSERT_EQ(world.GetSystemCount(), 1);
    ASSERT_EQ(world.GetSystemAccess(0).size(), 2);
    EXPECT_EQ(world.GetSystemAccess(0)[0].Mode, NEcs::ESystemAccessMode::READ);
    EXPECT_EQ(world.GetSystemAccess(0)[1].Mode, NEcs::ESystemAccessMode::WRITE);

    world.RunSystems();

    EXPECT_EQ(world.GetComponent<Position>(entity).X, 6);
}

TEST(EcsWorld, RejectsInvalidSystemRegistrationAndAccess) {
    NEcs::World world;

    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { world.RegisterSystem({}, {}); });
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { static_cast<void>(world.GetSystemAccess(0)); });
}

TEST(EcsWorld, ReportsMissingComponentThroughProjectError) {
    NEcs::World world;
    const NEcs::Entity entity = world.CreateEntity();

    NTest::ExpectError(NCommon::EError::NOT_FOUND, [&] { static_cast<void>(world.GetComponent<Position>(entity)); });
}
