#include <array>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

#include <application/service_manager.h>
#include <gtest/gtest.h>
#include <tests/common/test_error.h>

namespace {

using NApplication::EServiceState;
using NApplication::ServiceContext;
using NApplication::ServiceManager;
using NApplication::ServiceReference;

static_assert(!std::is_move_constructible_v<ServiceManager>);
static_assert(!std::is_copy_constructible_v<ServiceManager>);
static_assert(std::same_as<decltype(std::declval<const ServiceManager&>().Get<int>()), const int&>);
static_assert(
        std::same_as<decltype(std::declval<const ServiceManager&>().Observe<int>()), ServiceReference<const int>>);

struct Events {
    std::vector<int> Values;
};

template<int Id>
struct Service {
    explicit Service(Events& events)
        : Log(events) {
    }
    ~Service() {
        Log.Values.push_back(100 + Id);
    }
    void Start(const ServiceContext& context) {
        EXPECT_EQ(context.Services.GetState(), EServiceState::STARTING);
        EXPECT_EQ(context.Generation, context.Services.GetGeneration());
        Generation = context.Generation;
        Log.Values.push_back(Id);
        if (FailStart) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::IO_ERROR, "Startup failure {}", Id);
        }
    }
    void Stop() {
        Log.Values.push_back(-Id);
        if (FailStop) {
            throw std::runtime_error{"Stop failure"};
        }
    }
    Events& Log;
    std::uint64_t Generation = 0;
    bool FailStart = false;
    bool FailStop = false;
};

TEST(ServiceManager, UniqueTypedOwnershipConstAccessAndStableAddress) {
    ServiceManager manager;
    auto& value = manager.Register<std::unique_ptr<int>>(std::make_unique<int>(42));
    auto* address = &value;
    EXPECT_EQ(manager.TryGet<float>(), nullptr);
    NTest::ExpectError(NCommon::EError::NOT_FOUND, [&] { (void)manager.Get<float>(); });
    NTest::ExpectError(NCommon::EError::ALREADY_EXISTS, [&] { manager.Register<std::unique_ptr<int>>(); });
    manager.Register<int>(7);
    EXPECT_EQ(manager.TryGet<std::unique_ptr<int>>(), address);
    const auto& immutable = manager;
    EXPECT_EQ(**immutable.TryGet<std::unique_ptr<int>>(), 42);
    EXPECT_EQ(immutable.Get<int>(), 7);
    EXPECT_EQ(manager.Start(), 1);
    EXPECT_TRUE(manager.Stop());
    EXPECT_EQ(manager.Start(), 2);
    EXPECT_EQ(&manager.Get<std::unique_ptr<int>>(), address);
    EXPECT_TRUE(manager.Stop());
}

TEST(ServiceManager, ValidatesWholeDagBeforeAnyStartupAndAllowsConfigurationRepair) {
    Events events;
    ServiceManager manager;
    manager.Register<Service<1>>(events);
    manager.Register<Service<2>, Service<3>>(events);
    NTest::ExpectError(NCommon::EError::NOT_FOUND, [&] { (void)manager.Start(); });
    EXPECT_TRUE(events.Values.empty());
    EXPECT_EQ(manager.GetGeneration(), 0);
    EXPECT_EQ(manager.GetState(), EServiceState::CONFIGURING);
    manager.Register<Service<3>>(events);
    EXPECT_EQ(manager.Start(), 1);
    EXPECT_EQ(events.Values, (std::vector<int>{1, 3, 2}));
    EXPECT_TRUE(manager.Stop());
}

TEST(ServiceManager, CyclesAndSelfDependenciesHaveNoLifecycleSideEffects) {
    Events events;
    {
        ServiceManager manager;
        manager.Register<Service<1>>(events);
        manager.Register<Service<2>, Service<3>>(events);
        manager.Register<Service<3>, Service<2>>(events);
        NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)manager.Start(); });
        EXPECT_TRUE(events.Values.empty());
    }
    events.Values.clear();
    ServiceManager manager;
    manager.Register<Service<1>, Service<1>>(events);
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)manager.Start(); });
    EXPECT_TRUE(events.Values.empty());
}

TEST(ServiceManager, DeterministicDependencyOrderReverseShutdownAndDestruction) {
    Events events;
    events.Values.reserve(40);
    {
        ServiceManager manager;
        manager.Register<Service<3>, Service<1>, Service<2>>(events);
        manager.Register<Service<2>>(events);
        manager.Register<Service<1>>(events);
        manager.Register<Service<4>, Service<2>, Service<2>>(events);
        for (std::uint64_t generation = 1; generation <= 2; ++generation) {
            EXPECT_EQ(manager.Start(), generation);
            EXPECT_EQ(manager.Get<Service<3>>().Generation, generation);
            EXPECT_TRUE(manager.Stop());
            EXPECT_TRUE(manager.Stop());
        }
        EXPECT_EQ(events.Values, (std::vector<int>{2, 1, 3, 4, -4, -3, -1, -2, 2, 1, 3, 4, -4, -3, -1, -2}));
    }
    EXPECT_EQ(events.Values,
              (std::vector<int>{2, 1, 3, 4, -4, -3, -1, -2, 2, 1, 3, 4, -4, -3, -1, -2, 104, 103, 101, 102}));
}

TEST(ServiceManager, EveryStartupFailureRollsBackOnlySuccessfulPredecessorsAndCanRetry) {
    for (int failing = 1; failing <= 3; ++failing) {
        Events events;
        ServiceManager manager;
        auto& first = manager.Register<Service<1>>(events);
        auto& second = manager.Register<Service<2>, Service<1>>(events);
        auto& third = manager.Register<Service<3>, Service<2>>(events);
        first.FailStart = failing == 1;
        second.FailStart = failing == 2;
        third.FailStart = failing == 3;
        NTest::ExpectError(NCommon::EError::IO_ERROR, [&] { (void)manager.Start(); });
        EXPECT_EQ(manager.GetState(), EServiceState::STOPPED);
        ASSERT_NE(manager.GetFailure(), nullptr);
        NTest::ExpectError(NCommon::EError::IO_ERROR, [&] { std::rethrow_exception(manager.GetFailure()); });
        std::vector<int> expected;
        for (int index = 1; index <= failing; ++index) {
            expected.push_back(index);
        }
        for (int index = failing - 1; index >= 1; --index) {
            expected.push_back(-index);
        }
        EXPECT_EQ(events.Values, expected);
        EXPECT_TRUE(manager.Stop());
        EXPECT_EQ(events.Values, expected);
        first.FailStart = second.FailStart = third.FailStart = false;
        EXPECT_EQ(manager.Start(), 2);
        EXPECT_EQ(manager.GetFailure(), nullptr);
        EXPECT_TRUE(manager.Stop());
    }
}

TEST(ServiceManager, StartupErrorSurvivesMultipleRollbackErrorsAndRestartIsClosed) {
    Events events;
    ServiceManager manager;
    manager.Register<Service<1>>(events).FailStop = true;
    manager.Register<Service<2>, Service<1>>(events).FailStop = true;
    manager.Register<Service<3>, Service<2>>(events).FailStart = true;
    NTest::ExpectError(NCommon::EError::IO_ERROR, [&] { (void)manager.Start(); });
    EXPECT_EQ(events.Values, (std::vector<int>{1, 2, 3, -2, -1}));
    EXPECT_EQ(manager.GetState(), EServiceState::FAILED);
    NTest::ExpectError(NCommon::EError::IO_ERROR, [&] { std::rethrow_exception(manager.GetFailure()); });
    const auto failures = manager.GetStopFailures();
    ASSERT_EQ(failures.size(), 3);
    EXPECT_EQ(failures[0].Type, typeid(Service<1>));
    EXPECT_NE(failures[0].Failure, nullptr);
    EXPECT_NE(failures[1].Failure, nullptr);
    EXPECT_EQ(failures[2].Failure, nullptr);
    EXPECT_FALSE(manager.Stop());
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)manager.Start(); });
}

TEST(ServiceManager, TeardownContinuesAfterErrorsAndPreservesFirstFailure) {
    Events events;
    ServiceManager manager;
    manager.Register<Service<1>>(events).FailStop = true;
    manager.Register<Service<2>, Service<1>>(events).FailStop = true;
    manager.Start();
    EXPECT_FALSE(manager.Stop());
    EXPECT_EQ(events.Values, (std::vector<int>{1, 2, -2, -1}));
    EXPECT_EQ(manager.GetFailure(), manager.GetStopFailures()[1].Failure);
    EXPECT_FALSE(manager.Stop());
    EXPECT_EQ(events.Values.size(), 4);
}

struct ConstructorFailure {
    explicit ConstructorFailure(std::unique_ptr<int> value) {
        (void)value;
        throw std::runtime_error{"Constructor failure"};
    }
};

TEST(ServiceManager, ConstructorFailureDoesNotPublishOrCloseRegistration) {
    ServiceManager manager;
    EXPECT_THROW(manager.Register<ConstructorFailure>(std::make_unique<int>(1)), std::runtime_error);
    EXPECT_EQ(manager.TryGet<ConstructorFailure>(), nullptr);
    EXPECT_EQ(manager.GetState(), EServiceState::CONFIGURING);
    EXPECT_EQ(manager.Register<int>(9), 9);
    EXPECT_EQ(manager.Start(), 1);
    EXPECT_TRUE(manager.Stop());
}

TEST(ServiceManager, ReferencesExpireWithoutRetainingServiceAndRemainValidAcrossRuns) {
    ServiceReference<std::unique_ptr<int>> reference;
    ServiceReference<const std::unique_ptr<int>> immutable;
    EXPECT_EQ(reference.TryGet(), nullptr);
    {
        ServiceManager manager;
        manager.Register<std::unique_ptr<int>>(std::make_unique<int>(11));
        reference = manager.Observe<std::unique_ptr<int>>();
        immutable = std::as_const(manager).Observe<std::unique_ptr<int>>();
        EXPECT_EQ(*reference.Get(), 11);
        EXPECT_EQ(manager.Observe<float>().TryGet(), nullptr);
        manager.Start();
        EXPECT_TRUE(manager.Stop());
        manager.Start();
        EXPECT_EQ(*immutable.Get(), 11);
    }
    EXPECT_EQ(reference.TryGet(), nullptr);
    EXPECT_EQ(immutable.TryGet(), nullptr);
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)reference.Get(); });
}

struct ReentrantService {
    void Start(const ServiceContext& context) {
        Owner = &context.Services;
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { Owner->Register<int>(); });
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)Owner->Start(); });
        EXPECT_FALSE(Owner->Stop());
    }
    void Stop() const {
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)Owner->Start(); });
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { Owner->Register<int>(); });
        EXPECT_FALSE(Owner->Stop());
    }
    ServiceManager* Owner = nullptr;
};

struct ReentrantConstructor {
    explicit ReentrantConstructor(ServiceManager& manager) {
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { manager.Register<int>(); });
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)manager.Start(); });
        EXPECT_FALSE(manager.Stop());
    }
};

TEST(ServiceManager, LifecycleAndConstructionReentrancyAreRejected) {
    ServiceManager manager;
    manager.Register<ReentrantConstructor>(manager);
    manager.Register<ReentrantService>();
    manager.Start();
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)manager.Start(); });
    EXPECT_TRUE(manager.Stop());
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { manager.Register<int>(); });
}

TEST(ServiceManager, FrozenRegistrySupportsConcurrentReadOnlyLookupAndRejectsForeignLifecycle) {
    ServiceManager manager;
    manager.Register<int>(31);
    manager.Start();
    std::array<std::thread, 8> readers;
    std::atomic<int> observed{0};
    for (auto& reader: readers) {
        reader = std::thread{[&] {
            const auto& immutable = manager;
            for (int iteration = 0; iteration < 1000; ++iteration) {
                if (immutable.Get<int>() == 31 && immutable.Observe<int>().Get() == 31) {
                    ++observed;
                }
            }
            NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { manager.Register<float>(); });
            NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)manager.Start(); });
            EXPECT_FALSE(manager.Stop());
        }};
    }
    for (auto& reader: readers) {
        reader.join();
    }
    EXPECT_EQ(observed.load(), 8000);
    EXPECT_TRUE(manager.Stop());
}

TEST(ServiceManager, EmptyRegistryRestartsAndStopBeforeStartKeepsConfigurationOpen) {
    ServiceManager manager;
    EXPECT_TRUE(manager.Stop());
    EXPECT_EQ(manager.GetState(), EServiceState::CONFIGURING);
    EXPECT_EQ(manager.Start(), 1);
    EXPECT_TRUE(manager.Stop());
    EXPECT_EQ(manager.Start(), 2);
    EXPECT_TRUE(manager.Stop());
}

TEST(ServiceManager, DestructorStopsRunningServicesWithoutPropagatingTeardownErrors) {
    Events events;
    events.Values.reserve(8);
    {
        ServiceManager manager;
        manager.Register<Service<2>, Service<1>>(events).FailStop = true;
        manager.Register<Service<1>>(events);
        manager.Start();
    }
    EXPECT_EQ(events.Values, (std::vector<int>{1, 2, -2, -1, 102, 101}));
}

} // namespace
