#pragma once

#include <atomic>
#include <concepts>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <GraphicsEngine/lib/common/error/error.h>
#include <GraphicsEngine/lib/common/error/exception.h>
#include <GraphicsEngine/lib/common/wrapper/non_copyable.h>

namespace NCommon {

template<typename Function>
concept UserJobFunction =
        std::constructible_from<std::decay_t<Function>, Function&&> && std::invocable<std::decay_t<Function>> &&
        !std::is_reference_v<std::invoke_result_t<std::decay_t<Function>>> &&
        (std::is_void_v<std::invoke_result_t<std::decay_t<Function>>> ||
         (std::is_move_constructible_v<std::invoke_result_t<std::decay_t<Function>>> &&
          std::is_nothrow_destructible_v<std::invoke_result_t<std::decay_t<Function>>>));

namespace NJobInternal {

template<typename T>
struct JobState {
    std::optional<std::expected<T, ErrorInfo>> Result;
    ErrorInfo AllocationFailure{.Code = EError::OUT_OF_MEMORY, .Message = "User job allocation failed"};
    std::atomic_bool Ready = false;
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
    Job& operator=(Job&&) noexcept = default;

    [[nodiscard]] bool IsReady() const noexcept {
        return m_state && m_state->Ready.load(std::memory_order_acquire);
    }

    // Nullopt before completion; expected<T, ErrorInfo> once after completion (including void/errors).
    // Successful consumption invalidates the handle. Repeated/invalid consumption is INVALID_STATE.
    // A throwing T move also consumes the handle; publication is never repeated.
    [[nodiscard]] std::optional<std::expected<T, ErrorInfo>> TryTake() {
        if (!m_state) {
            GRAPHICS_ENGINE_THROW(EError::INVALID_STATE, "Cannot consume an invalid or consumed Job");
        }
        if (!m_state->Ready.load(std::memory_order_acquire)) {
            return std::nullopt;
        }
        const std::shared_ptr<NJobInternal::JobState<T>> state = std::move(m_state);
        return std::optional<std::expected<T, ErrorInfo>>{std::in_place, std::move(*state->Result)};
    }

private:
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
    template<typename Submit, UserJobFunction Function>
    [[nodiscard]] static auto Run(Submit&& submit, Function&& function) {
        using Callable = std::decay_t<Function>;
        using Value = std::invoke_result_t<Callable>;
        const std::shared_ptr<JobState<Value>> state = std::make_shared<JobState<Value>>();
        std::shared_ptr<Callable> callable = std::make_shared<Callable>(std::forward<Function>(function));
        std::invoke(std::forward<Submit>(submit), [completion = state, execution = std::move(callable)]() mutable {
            // Empty the executor closure before invoking user code. User captures/results
            // are released here, outside any scheduler/engine terminal-state locks.
            const std::shared_ptr<JobState<Value>> ownedState = std::move(completion);
            std::shared_ptr<Callable> ownedFunction = std::move(execution);
            try {
                if constexpr (std::is_void_v<Value>) {
                    std::invoke(std::move(*ownedFunction));
                    ownedState->Result.emplace();
                } else {
                    ownedState->Result.emplace(std::in_place, std::invoke(std::move(*ownedFunction)));
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
            ownedState->Ready.store(true, std::memory_order_release);
        });
        return Job<Value>{std::move(state)};
    }
};

} // namespace NJobInternal

} // namespace NCommon
