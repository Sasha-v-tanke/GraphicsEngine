#pragma once

#include <atomic>
#include <concepts>
#include <coroutine>
#include <cstdio>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <GraphicsEngine/lib/common/error/error.h>
#include <GraphicsEngine/lib/common/error/exception.h>
#include <GraphicsEngine/lib/common/wrapper/non_copyable.h>
#include <GraphicsEngine/lib/thread/job_control.h>
#include <GraphicsEngine/lib/thread/user_task.h>

namespace NCommon {

namespace NJobInternal {

template<typename Function>
struct FunctionResult: std::invoke_result<std::decay_t<Function>> {};

template<typename Function>
    requires std::invocable<std::decay_t<Function>, std::stop_token>
struct FunctionResult<Function>: std::invoke_result<std::decay_t<Function>, std::stop_token> {};

} // namespace NJobInternal

template<typename Function>
concept UserJobFunction =
        std::constructible_from<std::decay_t<Function>, Function&&> &&
        (std::invocable<std::decay_t<Function>> || std::invocable<std::decay_t<Function>, std::stop_token>) &&
        !std::is_reference_v<typename NJobInternal::FunctionResult<Function>::type> &&
        (std::is_void_v<typename NJobInternal::FunctionResult<Function>::type> ||
         (std::is_move_constructible_v<typename NJobInternal::FunctionResult<Function>::type> &&
          std::is_nothrow_destructible_v<typename NJobInternal::FunctionResult<Function>::type>));

namespace NJobInternal {

template<typename T>
class JobState final: public JobControl {
public:
    std::optional<std::expected<T, ErrorInfo>> Result;
    ErrorInfo AllocationFailure{.Code = EError::OUT_OF_MEMORY, .Message = "User job allocation failed"};
    ErrorInfo Cancellation{.Code = EError::CANCELLED, .Message = "User job cancelled"};
    std::function<bool()> Body;

private:
    bool ExecuteBody() noexcept override {
        const bool completed = Body(); // Factory catches user exceptions, including result construction.
        if (completed) {
            ReleaseBody(); // Coroutine frame/captures retire before result publication.
        }
        return completed;
    }
    void SchedulingFailure() noexcept override {
        Result.emplace(std::unexpect, std::move(AllocationFailure));
    }
    void ReleaseBody() noexcept override {
        Body = {};
    }
    void CancelResult() noexcept override {
        // A real worker failure remains observable even if cancellation races with it.
        if (!Result || Result->has_value()) {
            Result.emplace(std::unexpect, std::move(Cancellation));
        }
    }
    void ReportFailure() const noexcept override {
        if (Result && !Result->has_value() && Result->error().Code != EError::CANCELLED) {
            std::fprintf(stderr,
                         "GraphicsEngine unobserved user job failure [%d]: %s\n",
                         Result->error().Code.value(),
                         Result->error().Message.c_str());
        }
    }
};

struct JobFactory;

} // namespace NJobInternal

// Single consumer of a retained result. Destroying a handle does not cancel its work.
// IsReady/TryTake never wait. Concurrent consumption/move of the same handle is unsupported.
// Job<T&> is deliberately unsupported; borrowed results must have explicit owner lifetime.
template<typename T>
class [[nodiscard]] Job final: private NonCopyable {
    static_assert(!std::is_reference_v<T>, "Job cannot hold a reference result");

public:
    Job() = default;
    Job(Job&&) noexcept = default;
    Job& operator=(Job&& other) noexcept {
        if (this != &other) {
            Abandon();
            m_state = std::move(other.m_state);
        }
        return *this;
    }
    ~Job() {
        Abandon();
    }

    // Asynchronous request: no user stop_callback runs on the caller. Invalid handles throw INVALID_STATE.
    void RequestCancel() {
        if (!m_state) {
            GRAPHICS_ENGINE_THROW(EError::INVALID_STATE, "Cannot cancel an invalid or consumed Job");
        }
        m_state->RequestCancel();
    }

    [[nodiscard]] bool IsReady() const noexcept {
        return m_state && m_state->IsReady();
    }

    // Nullopt before completion; expected<T, ErrorInfo> once after completion (including void/errors).
    // Successful consumption invalidates the handle. Repeated/invalid consumption is INVALID_STATE.
    // A throwing T move also consumes the handle; publication is never repeated.
    [[nodiscard]] std::optional<std::expected<T, ErrorInfo>> TryTake() {
        if (!m_state) {
            GRAPHICS_ENGINE_THROW(EError::INVALID_STATE, "Cannot consume an invalid or consumed Job");
        }
        if (!m_state->IsReady()) {
            return std::nullopt;
        }
        m_state->Observed.store(true);
        const std::shared_ptr<NJobInternal::JobState<T>> state = std::move(m_state);
        return std::optional<std::expected<T, ErrorInfo>>{std::in_place, std::move(*state->Result)};
    }

    class Awaiter final: private NonCopyable {
    public:
        explicit Awaiter(std::shared_ptr<NJobInternal::JobState<T>> state)
            : m_state(std::move(state)) {
        }
        ~Awaiter() {
            if (m_state) {
                m_state->Abandon();
            }
        }
        [[nodiscard]] bool await_ready() const noexcept {
            return m_state->IsReady();
        }
        template<typename Promise>
            requires std::derived_from<Promise, NUserTaskInternal::PromiseBase>
        bool await_suspend(std::coroutine_handle<Promise> handle) {
            const std::shared_ptr<NJobInternal::JobControl> owner = handle.promise().Owner.lock();
            if (!owner) {
                GRAPHICS_ENGINE_THROW(EError::INVALID_STATE, "Job await requires a spawned UserTask");
            }
            m_continuation = std::make_shared<NJobInternal::JobContinuation>();
            m_continuation->Owner = owner;
            owner->SetWaiting(m_continuation);
            // No raw coroutine address escapes. Resume is allowed only after the current step returns.
            return m_state->Subscribe(m_continuation);
        }
        T await_resume() {
            m_state->Observed.store(true);
            const std::shared_ptr<NJobInternal::JobState<T>> state = std::move(m_state);
            std::expected<T, ErrorInfo>& result = *state->Result;
            if (!result) {
                GRAPHICS_ENGINE_THROW(result.error().Code, "{}", result.error().Message);
            }
            if constexpr (!std::is_void_v<T>) {
                return std::move(*result);
            }
        }

    private:
        std::shared_ptr<NJobInternal::JobState<T>> m_state;
        std::shared_ptr<NJobInternal::JobContinuation> m_continuation;
    };

    // Single-consumer ownership transfers at the await expression, even when already ready.
    [[nodiscard]] Awaiter operator co_await() && {
        if (!m_state) {
            GRAPHICS_ENGINE_THROW(EError::INVALID_STATE, "Cannot await an invalid or consumed Job");
        }
        return Awaiter{std::move(m_state)};
    }
    Awaiter operator co_await() & = delete;

private:
    void Abandon() noexcept {
        if (m_state) {
            m_state->Abandon();
        }
    }
    explicit Job(std::shared_ptr<NJobInternal::JobState<T>> state)
        : m_state(std::move(state)) {
    }

private:
    std::shared_ptr<NJobInternal::JobState<T>> m_state;
    friend struct NJobInternal::JobFactory;
};

namespace NJobInternal {

template<typename T>
void SetFailure(JobState<T>& state, std::error_code code, std::string_view message) noexcept {
    ErrorInfo error = std::move(state.AllocationFailure);
    try {
        error = ErrorInfo{.Code = code, .Message = std::string{message}};
    } catch (...) {
        // The completion state already owns an allocation-failure diagnostic.
    }
    state.Result.emplace(std::unexpect, std::move(error));
}

struct JobFactory {
    template<typename Submit, typename T>
    [[nodiscard]] static Job<T> Spawn(Submit&& submit, UserTask<T>&& task) {
        if (!task.IsValid()) {
            GRAPHICS_ENGINE_THROW(EError::INVALID_STATE, "Cannot spawn an invalid or already spawned UserTask");
        }
        // Consume the lazy frame before any admission/allocation failure, as for the original Spawn contract.
        UserTask<T> owned = std::move(task);
        const std::shared_ptr<JobState<T>> state = std::make_shared<JobState<T>>();
        std::shared_ptr<UserTask<T>> frame = std::make_shared<UserTask<T>>(std::move(owned));
        state->Body = [completion = state.get(),
                       owner = std::weak_ptr<JobControl>{state},
                       frame = std::move(frame)]() mutable {
            try {
                if (!NUserTaskInternal::TaskAccess::Resume(*frame, completion->StopSource.get_token(), owner)) {
                    return false;
                }
                if constexpr (std::is_void_v<T>) {
                    NUserTaskInternal::TaskAccess::Take(*frame);
                    completion->Result.emplace();
                } else {
                    completion->Result.emplace(std::in_place, NUserTaskInternal::TaskAccess::Take(*frame));
                }
            } catch (const Exception& exception) {
                SetFailure(*completion, exception.code(), exception.GetMessage());
            } catch (const std::bad_alloc&) {
                completion->Result.emplace(std::unexpect, std::move(completion->AllocationFailure));
            } catch (const std::exception& exception) {
                SetFailure(*completion, make_error_code(EError::UNKNOWN), exception.what());
            } catch (...) {
                SetFailure(*completion,
                           make_error_code(EError::UNKNOWN),
                           "User coroutine threw a non-standard exception");
            }
            return true;
        };
        std::invoke(std::forward<Submit>(submit), state);
        return Job<T>{state};
    }

    template<typename Submit, UserJobFunction Function>
    [[nodiscard]] static auto Run(Submit&& submit, Function&& function) {
        using Callable = std::decay_t<Function>;
        using Value = typename FunctionResult<Function>::type;
        const std::shared_ptr<JobState<Value>> state = std::make_shared<JobState<Value>>();
        std::shared_ptr<Callable> callable = std::make_shared<Callable>(std::forward<Function>(function));
        // The owning state keeps this private closure alive for one claimed execution.
        state->Body = [completion = state.get(), execution = std::move(callable)]() mutable {
            JobState<Value>* ownedState = completion;
            std::shared_ptr<Callable> ownedFunction = std::move(execution);
            const auto invoke = [&] {
                if constexpr (std::invocable<Callable, std::stop_token>) {
                    return std::invoke(std::move(*ownedFunction), ownedState->StopSource.get_token());
                } else {
                    return std::invoke(std::move(*ownedFunction));
                }
            };
            try {
                if constexpr (std::is_void_v<Value>) {
                    invoke();
                    ownedState->Result.emplace();
                } else {
                    ownedState->Result.emplace(std::in_place, invoke());
                }
            } catch (const Exception& exception) {
                SetFailure(*ownedState, exception.code(), exception.GetMessage());
            } catch (const std::bad_alloc&) {
                ownedState->Result.emplace(std::unexpect, std::move(ownedState->AllocationFailure));
            } catch (const std::exception& exception) {
                SetFailure(*ownedState, make_error_code(EError::UNKNOWN), exception.what());
            } catch (...) {
                SetFailure(*ownedState, make_error_code(EError::UNKNOWN), "User job threw a non-standard exception");
            }
            ownedFunction.reset();
            return true;
        };
        std::invoke(std::forward<Submit>(submit), state);
        return Job<Value>{std::move(state)};
    }
};

} // namespace NJobInternal

} // namespace NCommon
