#pragma once

#include <concepts>
#include <coroutine>
#include <exception>
#include <memory>
#include <optional>
#include <stop_token>
#include <type_traits>
#include <utility>

#include <GraphicsEngine/lib/common/error/exception.h>
#include <GraphicsEngine/lib/common/wrapper/non_copyable.h>
#include <GraphicsEngine/lib/thread/job_control.h>

namespace NCommon {

// Ready-only query inside UserTask: co_await UserTaskStopToken{} yields its runtime stop token.
struct UserTaskStopToken {};

template<typename T>
class UserTask;
template<typename T>
class Job;

namespace NUserTaskInternal {

struct TaskAccess;

struct TokenAwaiter {
    std::stop_token Token;
    // NOLINTNEXTLINE(readability-convert-member-functions-to-static): compiler await protocol uses an instance.
    [[nodiscard]] bool await_ready() const noexcept {
        return true;
    }
    void await_suspend(std::coroutine_handle<>) const noexcept {
    }
    [[nodiscard]] std::stop_token await_resume() const noexcept {
        return Token;
    }
};

struct PromiseBase {
    std::exception_ptr Exception;
    std::stop_token StopToken;
    std::weak_ptr<NJobInternal::JobControl> Owner;

    // NOLINTNEXTLINE(readability-convert-member-functions-to-static): compiler promise protocol uses an instance.
    [[nodiscard]] std::suspend_always initial_suspend() const noexcept {
        return {};
    }
    // NOLINTNEXTLINE(readability-convert-member-functions-to-static): compiler promise protocol uses an instance.
    [[nodiscard]] std::suspend_always final_suspend() const noexcept {
        return {};
    }
    void unhandled_exception() noexcept {
        Exception = std::current_exception();
    }
    [[nodiscard]] TokenAwaiter await_transform(UserTaskStopToken) const noexcept {
        return {StopToken};
    }
    template<typename T>
    [[nodiscard]] Job<T>&& await_transform(Job<T>&& job) noexcept {
        return std::move(job);
    }
    // Foreign suspension protocols cannot participate in runtime frame ownership.
    template<typename Awaitable>
        requires(!std::same_as<std::remove_cvref_t<Awaitable>, UserTaskStopToken>)
    void await_transform(Awaitable&&) = delete;
};

template<typename T>
struct Promise: PromiseBase {
    std::optional<T> Result;
    [[nodiscard]] UserTask<T> get_return_object() noexcept;

    template<typename Value>
        requires std::constructible_from<T, Value&&>
    void return_value(Value&& value) {
        Result.emplace(std::forward<Value>(value));
    }
};

template<>
struct Promise<void>: PromiseBase {
    [[nodiscard]] UserTask<void> get_return_object() noexcept;
    void return_void() const noexcept {
    }
};

} // namespace NUserTaskInternal

// Lazy, single-owner coroutine frame. Creation copies/moves parameters but does not run the body.
// Spawn consumes ownership; users cannot resume a frame independently of the runtime.
// Moving/spawning/destroying the same UserTask concurrently is unsupported.
// Borrowed parameters/this and coroutine-lambda captures must outlive execution, just like ordinary C++ coroutines.
template<typename T>
class [[nodiscard]] UserTask final: private NonCopyable {
    static_assert(!std::is_reference_v<T>, "UserTask cannot hold a reference result");
    static_assert(std::is_void_v<T> || (std::is_move_constructible_v<T> && std::is_nothrow_destructible_v<T>));

public:
    using promise_type = NUserTaskInternal::Promise<T>;

    UserTask() noexcept = default;
    UserTask(UserTask&& other) noexcept
        : m_handle(std::exchange(other.m_handle, {})) {
    }
    UserTask& operator=(UserTask&& other) noexcept {
        if (this != &other) {
            Reset();
            m_handle = std::exchange(other.m_handle, {});
        }
        return *this;
    }
    ~UserTask() {
        Reset();
    }
    [[nodiscard]] bool IsValid() const noexcept {
        return static_cast<bool>(m_handle);
    }

private:
    explicit UserTask(std::coroutine_handle<promise_type> handle) noexcept
        : m_handle(handle) {
    }
    void Reset() noexcept {
        if (m_handle) {
            m_handle.destroy();
            m_handle = {};
        }
    }

private:
    std::coroutine_handle<promise_type> m_handle;
    friend struct NUserTaskInternal::Promise<T>;
    friend struct NUserTaskInternal::TaskAccess;
};

namespace NUserTaskInternal {

template<typename T>
UserTask<T> Promise<T>::get_return_object() noexcept {
    return UserTask<T>{std::coroutine_handle<Promise<T>>::from_promise(*this)};
}

inline UserTask<void> Promise<void>::get_return_object() noexcept {
    return UserTask<void>{std::coroutine_handle<Promise<void>>::from_promise(*this)};
}

struct TaskAccess {
    template<typename T>
    static bool Resume(UserTask<T>& task, std::stop_token token, const std::weak_ptr<NJobInternal::JobControl>& owner) {
        typename UserTask<T>::promise_type& promise = task.m_handle.promise();
        promise.StopToken = std::move(token);
        promise.Owner = owner;
        task.m_handle.resume();
        return task.m_handle.done();
    }

    template<typename T>
    static T Take(UserTask<T>& task) {
        typename UserTask<T>::promise_type& promise = task.m_handle.promise();
        if (promise.Exception) {
            std::rethrow_exception(promise.Exception);
        }
        if constexpr (!std::is_void_v<T>) {
            if (!promise.Result) {
                GRAPHICS_ENGINE_THROW(EError::INVALID_STATE, "UserTask completed without a result");
            }
            return std::move(*promise.Result);
        }
    }
};

} // namespace NUserTaskInternal

} // namespace NCommon
