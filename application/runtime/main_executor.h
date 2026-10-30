#pragma once

#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

#include <GraphicsEngine/lib/common/wrapper/non_transferable.h>

namespace NApplication::NRuntime {

enum class EMainCommandState {
    QUEUED,
    RUNNING,
    COMPLETED,
    FAILED,
    SKIPPED,
};

// Shared completion survives queue removal and executor shutdown. Waiting never executes CPU tasks.
class MainCompletion final {
public:
    [[nodiscard]] EMainCommandState GetState() const;
    [[nodiscard]] std::exception_ptr GetFailure() const;
    [[nodiscard]] EMainCommandState Wait() const;

private:
    struct State {
        std::mutex Mutex;
        std::condition_variable Changed;
        EMainCommandState Value = EMainCommandState::QUEUED;
        std::exception_ptr Failure;
        std::thread::id Consumer;
    };

    explicit MainCompletion(std::shared_ptr<State> state);
    std::shared_ptr<State> m_state;
    friend class MainExecutor;
};

// Start/Drain belong to the creating thread; Post/Stop may race on other threads.
// Producers must finish before destruction. Wakeup is a nonthrowing platform signal, never user work.
class MainExecutor final: private NCommon::NonTransferable {
public:
    using Generation = std::uint64_t;
    // libc++ in supported Apple toolchains has no std::move_only_function yet.
    class Command final {
    public:
        Command() = default;
        Command(Command&&) noexcept = default;
        Command& operator=(Command&&) noexcept = default;

        template<typename Function>
            requires(!std::same_as<std::remove_cvref_t<Function>, Command> &&
                     std::constructible_from<std::decay_t<Function>, Function> &&
                     std::invocable<std::decay_t<Function>&> &&
                     std::is_void_v<std::invoke_result_t<std::decay_t<Function>&>>)
        Command(Function&& function)
            : m_body(std::make_unique<Body<std::decay_t<Function>>>(std::forward<Function>(function))) {
        }

        explicit operator bool() const noexcept {
            return m_body != nullptr;
        }
        void operator()() {
            m_body->Invoke();
        }

    private:
        struct IBody {
            virtual ~IBody() = default;
            virtual void Invoke() = 0;
        };
        template<typename Function>
        struct Body final: IBody {
            explicit Body(Function function)
                : Value(std::move(function)) {
            }
            void Invoke() override {
                std::invoke(Value);
            }
            Function Value;
        };
        std::unique_ptr<IBody> m_body;
    };

    struct Wakeup {
        void* Context = nullptr;
        void (*Notify)(void*) noexcept = nullptr;
        explicit operator bool() const noexcept {
            return Notify != nullptr;
        }
        void operator()() const noexcept {
            Notify(Context);
        }
    };
    static constexpr std::size_t DISPATCH_BATCH = 64;

    MainExecutor();
    ~MainExecutor();

    Generation Start(Wakeup wakeup = {nullptr, nullptr});
    void Stop() noexcept;
    [[nodiscard]] Generation GetGeneration() const;
    [[nodiscard]] MainCompletion Post(Generation generation, Command command);
    std::size_t Drain(std::size_t limit = DISPATCH_BATCH);
    [[nodiscard]] std::size_t GetPendingCount() const;

private:
    struct Entry {
        Command Function;
        std::shared_ptr<MainCompletion::State> Completion;
    };

    static void Finish(Entry& entry, EMainCommandState state, std::exception_ptr failure = {});
    void ValidateConsumer() const;

private:
    const std::thread::id m_consumer;
    mutable std::mutex m_mutex;
    std::list<Entry> m_queue;
    Wakeup m_wakeup;
    Generation m_generation = 0;
    std::size_t m_stopping = 0;
    bool m_running = false;
    bool m_draining = false;
};

} // namespace NApplication::NRuntime
