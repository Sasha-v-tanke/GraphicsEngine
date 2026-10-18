#include <vector>

#include <GraphicsEngine/ecs/world.h>
#include <gtest/gtest.h>
#include <tests/common/test_error.h>

namespace {

struct Position {
    int Value = 0;
};

TEST(EcsScheduling, StableIdsDetermineSerialScheduleIndependentlyOfRegistration) {
    auto replay = [](bool reverseRegistration) {
        NEcs::World world;
        std::vector<int> order;
        auto low = [&] {
            world.RegisterSystem({}, [&](NEcs::World&) { order.push_back(10); }, NEcs::SystemSchedule{.Id = 10});
        };
        auto high = [&] {
            world.RegisterSystem({}, [&](NEcs::World&) { order.push_back(20); }, NEcs::SystemSchedule{.Id = 20});
        };
        if (reverseRegistration) {
            high();
            low();
        } else {
            low();
            high();
        }
        world.RunSystems();
        return order;
    };
    EXPECT_EQ(replay(false), (std::vector<int>{10, 20}));
    EXPECT_EQ(replay(false), replay(true));
}

TEST(EcsScheduling, ExplicitOrderingOverridesStableIdForConflictingAccess) {
    NEcs::World world;
    const auto entity = world.CreateEntity();
    world.AddComponent<Position>(entity);
    world.RegisterSystem(
            {NEcs::Read<Position>()},
            [&](NEcs::World& current) { EXPECT_EQ(current.GetComponent<Position>(entity).Value, 42); },
            NEcs::SystemSchedule{.Id = 10, .After = {20}});
    world.RegisterSystem(
            {NEcs::Write<Position>()},
            [&](NEcs::World& current) { current.GetComponent<Position>(entity).Value = 42; },
            NEcs::SystemSchedule{.Id = 20});
    world.RunSystems();
}

TEST(EcsScheduling, BeforeAndAfterConstraintsComposeWithoutImplicitRegistrationEdges) {
    NEcs::World world;
    std::vector<int> order;
    world.RegisterSystem({}, [&](NEcs::World&) { order.push_back(10); }, NEcs::SystemSchedule{.Id = 10, .After = {30}});
    world.RegisterSystem(
            {},
            [&](NEcs::World&) { order.push_back(20); },
            NEcs::SystemSchedule{.Id = 20, .Before = {30}});
    world.RegisterSystem({}, [&](NEcs::World&) { order.push_back(30); }, NEcs::SystemSchedule{.Id = 30});
    world.RunSystems();
    EXPECT_EQ(order, (std::vector<int>{20, 30, 10}));
}

TEST(EcsScheduling, RejectsCyclesAndUnknownIdsBeforeExecutingAnyCallback) {
    NEcs::World cyclic;
    int calls = 0;
    cyclic.RegisterSystem({}, [&](NEcs::World&) { ++calls; }, NEcs::SystemSchedule{.Id = 10, .After = {20}});
    cyclic.RegisterSystem({}, [&](NEcs::World&) { ++calls; }, NEcs::SystemSchedule{.Id = 20, .After = {10}});
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { cyclic.RunSystems(); });
    EXPECT_EQ(calls, 0);

    NEcs::World unknown;
    unknown.RegisterSystem({}, [&](NEcs::World&) { ++calls; }, NEcs::SystemSchedule{.Id = 10, .Before = {20}});
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { unknown.RunSystems(); });
    EXPECT_EQ(calls, 0);
}

TEST(EcsScheduling, RejectsDuplicateSchedulingIdsAndMixedRegistrationModes) {
    NEcs::World world;
    world.RegisterSystem({}, [](NEcs::World&) {}, NEcs::SystemSchedule{.Id = 10});
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT,
                       [&] { world.RegisterSystem({}, [](NEcs::World&) {}, NEcs::SystemSchedule{.Id = 10}); });
    EXPECT_EQ(world.GetSystemCount(), 1U);
    world.RegisterSystem({}, [](NEcs::World&) {});
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { world.RunSystems(); });
}

} // namespace
