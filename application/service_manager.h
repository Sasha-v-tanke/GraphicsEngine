#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <span>
#include <thread>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <vector>

#include <GraphicsEngine/lib/common/error/error.h>
#include <GraphicsEngine/lib/common/error/exception.h>
#include <GraphicsEngine/lib/common/wrapper/non_transferable.h>

namespace NApplication {

class ServiceManager;

struct ServiceContext {
    ServiceManager& Services;
    std::uint64_t Generation;
};

enum class EServiceState {
    CONFIGURING,
    STARTING,
    RUNNING,
    STOPPING,
    STOPPED,
    FAILED,
};

struct ServiceStopFailure {
    std::type_index Type;
    std::exception_ptr Failure;
};

// Non-owning, checked access. Does not synchronize destruction with callers.
template<typename T>
class ServiceReference final {
public:
    ServiceReference() = default;

    [[nodiscard]] T* TryGet() const noexcept {
        return m_lifetime.expired() ? nullptr : m_value;
    }

    [[nodiscard]] T& Get() const {
        if (auto* value = TryGet()) {
            return *value;
        }
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Service owner no longer exists");
    }

private:
    friend class ServiceManager;
    ServiceReference(std::weak_ptr<const void> lifetime, T* value)
        : m_lifetime(std::move(lifetime))
        , m_value(value) {
    }

    std::weak_ptr<const void> m_lifetime;
    T* m_value = nullptr;
};

// Register/Start/Stop/destruction belong to the creating thread and must not overlap readers.
// Once configuration closes, Get/TryGet/Observe may run concurrently. Service affinity still applies.
// Services must synchronously drain their consumers in Stop; destructors must not throw.
class ServiceManager final: private NCommon::NonTransferable {
public:
    ServiceManager() = default;
    ~ServiceManager();

    // Dependencies may be registered later, but must all exist before Start.
    // Optional hooks: void Start(const ServiceContext&) and void Stop(). A throwing Start
    // must undo its own partial work; the manager rolls back only successfully started services.
    template<typename T, typename... Dependencies, typename... Args>
        requires(std::same_as<T, std::remove_cvref_t<T>> && std::is_object_v<T> && !std::is_array_v<T> &&
                 std::constructible_from<T, Args...> &&
                 (std::same_as<Dependencies, std::remove_cvref_t<Dependencies>> && ...))
    T& Register(Args&&... args) {
        RegistrationGuard guard{*this, typeid(T)};
        auto entry = std::make_unique<Entry<T>>(std::vector<std::type_index>{typeid(Dependencies)...},
                                                std::forward<Args>(args)...);
        T& value = entry->Value;
        m_entries.push_back(std::move(entry));
        return value;
    }

    template<typename T>
    [[nodiscard]] T* TryGet() noexcept {
        auto* entry = Find(typeid(T));
        return entry == nullptr ? nullptr : static_cast<T*>(entry->GetValue());
    }

    template<typename T>
    [[nodiscard]] const T* TryGet() const noexcept {
        const auto* entry = Find(typeid(T));
        return entry == nullptr ? nullptr : static_cast<const T*>(entry->GetValue());
    }

    template<typename T>
    [[nodiscard]] T& Get() {
        if (auto* value = TryGet<T>()) {
            return *value;
        }
        GRAPHICS_ENGINE_THROW(NCommon::EError::NOT_FOUND, "Service is not registered: {}", typeid(T).name());
    }

    template<typename T>
    [[nodiscard]] const T& Get() const {
        if (const auto* value = TryGet<T>()) {
            return *value;
        }
        GRAPHICS_ENGINE_THROW(NCommon::EError::NOT_FOUND, "Service is not registered: {}", typeid(T).name());
    }

    template<typename T>
    [[nodiscard]] ServiceReference<T> Observe() noexcept {
        return {m_lifetime, TryGet<T>()};
    }

    template<typename T>
    [[nodiscard]] ServiceReference<const T> Observe() const noexcept {
        return {m_lifetime, TryGet<T>()};
    }

    // Validates the entire DAG before callbacks. Each attempt that invokes hooks gets a new generation.
    // The original startup exception propagates after reverse rollback, including engine Exceptions.
    std::uint64_t Start();
    // No allocations or exceptions. False means wrong thread, reentrant call, or failed teardown.
    // A teardown error permanently closes restart because consumer quiescence is not proven.
    [[nodiscard]] bool Stop() noexcept;

    // Lifecycle diagnostics require owner-thread access or external synchronization.
    [[nodiscard]] EServiceState GetState() const noexcept;
    [[nodiscard]] std::uint64_t GetGeneration() const noexcept;
    [[nodiscard]] std::exception_ptr GetFailure() const noexcept;
    // One slot per service in registration order; empty Failure means no teardown error.
    [[nodiscard]] std::span<const ServiceStopFailure> GetStopFailures() const noexcept;

private:
    struct IEntry {
        IEntry(std::type_index type, std::vector<std::type_index> dependencies);
        virtual ~IEntry() = default;
        virtual void Start(const ServiceContext& context) = 0;
        virtual void Stop() = 0;
        [[nodiscard]] virtual void* GetValue() noexcept = 0;
        [[nodiscard]] virtual const void* GetValue() const noexcept = 0;
        std::type_index Type;
        std::vector<std::type_index> Dependencies;
    };

    template<typename T>
    struct Entry final: IEntry {
        template<typename... Args>
        Entry(std::vector<std::type_index> dependencies, Args&&... args)
            : IEntry(typeid(T), std::move(dependencies))
            , Value(std::forward<Args>(args)...) {
        }

        void Start(const ServiceContext& context) override {
            if constexpr (requires { Value.Start(context); }) {
                static_assert(std::same_as<decltype(Value.Start(context)), void>, "Start must return void");
                Value.Start(context);
            }
        }

        void Stop() override {
            if constexpr (requires { Value.Stop(); }) {
                static_assert(std::same_as<decltype(Value.Stop()), void>, "Stop must return void");
                Value.Stop();
            }
        }

        [[nodiscard]] void* GetValue() noexcept override {
            return std::addressof(Value);
        }

        [[nodiscard]] const void* GetValue() const noexcept override {
            return std::addressof(Value);
        }

        T Value;
    };

    struct RegistrationGuard {
        RegistrationGuard(ServiceManager& manager, std::type_index type);
        ~RegistrationGuard();
        ServiceManager& Manager;
    };

    [[nodiscard]] IEntry* Find(std::type_index type) noexcept;
    [[nodiscard]] const IEntry* Find(std::type_index type) const noexcept;
    [[nodiscard]] std::vector<std::size_t> BuildOrder() const;
    void StopStarted() noexcept;
    void ValidateOwner() const;

private:
    const std::thread::id m_owner = std::this_thread::get_id();
    std::shared_ptr<const void> m_lifetime = std::make_shared<int>(0);
    std::vector<std::unique_ptr<IEntry>> m_entries;
    std::vector<std::size_t> m_order;
    std::vector<ServiceStopFailure> m_stopFailures;
    std::exception_ptr m_failure;
    std::uint64_t m_generation = 0;
    std::size_t m_started = 0;
    EServiceState m_state = EServiceState::CONFIGURING;
    bool m_registering = false;
};

} // namespace NApplication
