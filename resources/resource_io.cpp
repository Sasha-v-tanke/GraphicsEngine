#include "resource_io.h"

#include <array>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <fstream>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <GraphicsEngine/lib/common/error/exception.h>

namespace NResources {

namespace {

std::string ErrorMessage(const std::filesystem::path& path, std::string_view reason) {
    return "Resource I/O at '" + path.string() + "': " + std::string{reason};
}

ResourceIoBytes
ReadFile(const std::filesystem::path& path, const std::stop_token& cancellation, std::size_t maxReadBytes) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::IO_ERROR, "expected a readable regular file");
    }
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::IO_ERROR, "cannot open file");
    }
    ResourceIoBytes bytes;
    std::array<char, 64 * 1024> chunk{};
    while (!cancellation.stop_requested()) {
        file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const std::size_t count = static_cast<std::size_t>(file.gcount());
        if (count > maxReadBytes - bytes.size()) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "file exceeds configured byte limit");
        }
        const std::size_t offset = bytes.size();
        bytes.resize(offset + count);
        if (count != 0) {
            std::memcpy(bytes.data() + offset, chunk.data(), count);
        }
        if (file.bad() || (file.fail() && !file.eof())) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::IO_ERROR, "file read failed");
        }
        if (file.eof()) {
            break;
        }
    }
    return bytes;
}

} // namespace

struct ResourceIoRequest::State {
    explicit State(std::filesystem::path path)
        : Result{
                  .Path = std::move(path),
                  .Status = EResourceIoStatus::FAILED,
                  .Bytes = {},
                  .Failure = NCommon::ErrorInfo{.Code = NCommon::EError::OUT_OF_MEMORY,
                                                .Message = "Resource I/O allocation failed"},
          } {
        Result.Failure->Message = ErrorMessage(Result.Path, "allocation failed");
    }

    // Initialized before admission, so terminal publication itself does not need to allocate.
    ResourceIoResult Result;
    std::stop_source Cancellation;
    mutable std::mutex Mutex;
    bool Published = false;
    std::optional<NCommon::ErrorInfo> CompletionFailure;
};

struct ResourceIo::Impl {
    struct Job {
        std::shared_ptr<ResourceIoRequest::State> State;
        Completion OnComplete;
    };

    Impl(ResourceIoConfig config, ReadFunction reader)
        : Config(config)
        , Reader(reader ? std::move(reader) : ReadFile) {
        if (Config.WorkerCount == 0 || Config.MaxPendingReads == 0 || Config.MaxReadBytes == 0) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "ResourceIo limits must be positive");
        }
        Workers.reserve(Config.WorkerCount);
        try {
            for (std::size_t index = 0; index < Config.WorkerCount; ++index) {
                Workers.emplace_back([this] { Run(); });
            }
        } catch (...) {
            Shutdown();
            throw;
        }
    }

    ~Impl() {
        Shutdown();
    }

    void Shutdown() {
        if (CurrentWorker == this) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "ResourceIo cannot join its own worker");
        }
        {
            std::lock_guard lock{Mutex};
            Stopping = true;
        }
        // stop callbacks and completion captures may execute arbitrary code; hold no queue lock here.
        Cancellation.request_stop();
        Wakeup.notify_all();
        std::lock_guard joinLock{JoinMutex};
        for (std::thread& worker: Workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    static void SetFailure(ResourceIoRequest::State& state, std::error_code code, std::string_view reason) noexcept {
        try {
            state.Result.Failure = NCommon::ErrorInfo{code, ErrorMessage(state.Result.Path, reason)};
        } catch (...) {
            // Keep the preallocated OUT_OF_MEMORY error if even error reporting cannot allocate.
        }
    }

    void Execute(Job& job) const {
        ResourceIoRequest::State& state = *job.State;
        {
            std::stop_callback cancelOnShutdown{Cancellation.get_token(),
                                                [&state] { state.Cancellation.request_stop(); }};
            try {
                if (!state.Cancellation.stop_requested()) {
                    ResourceIoBytes bytes =
                            Reader(state.Result.Path, state.Cancellation.get_token(), Config.MaxReadBytes);
                    if (bytes.size() > Config.MaxReadBytes) {
                        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                                              "reader exceeded configured byte limit");
                    }
                    state.Result.Bytes = std::make_shared<const ResourceIoBytes>(std::move(bytes));
                    state.Result.Status = EResourceIoStatus::SUCCEEDED;
                    state.Result.Failure.reset();
                }
            } catch (const NCommon::Exception& exception) {
                SetFailure(state, exception.code(), exception.GetMessage());
            } catch (const std::bad_alloc&) {
                // The initial failure payload already describes allocation failure.
            } catch (const std::exception& exception) {
                SetFailure(state, NCommon::make_error_code(NCommon::EError::IO_ERROR), exception.what());
            } catch (...) {
                SetFailure(state, NCommon::make_error_code(NCommon::EError::UNKNOWN), "unknown reader exception");
            }
            if (state.Cancellation.stop_requested()) {
                state.Result.Status = EResourceIoStatus::CANCELLED;
                state.Result.Bytes.reset();
                state.Result.Failure.reset();
            }
        }
        {
            std::lock_guard lock{state.Mutex};
            state.Published = true;
        }
        if (job.OnComplete) {
            try {
                job.OnComplete(std::shared_ptr<const ResourceIoResult>{job.State, &state.Result});
            } catch (...) {
                std::lock_guard lock{state.Mutex};
                try {
                    state.CompletionFailure = NCommon::ErrorInfo{
                            .Code = NCommon::EError::UNKNOWN,
                            .Message = ErrorMessage(state.Result.Path, "completion callback threw"),
                    };
                } catch (...) {
                    // Read result remains valid even if the diagnostic allocation fails.
                }
            }
        }
    }

    void Run() {
        CurrentWorker = this;
        for (;;) {
            Job job;
            {
                std::unique_lock lock{Mutex};
                Wakeup.wait(lock, [this] { return Stopping || !Pending.empty(); });
                if (Pending.empty()) {
                    break;
                }
                job = std::move(Pending.front());
                Pending.pop_front();
            }
            Execute(job);
            job.OnComplete = {};
            job.State.reset();
            {
                std::lock_guard lock{Mutex};
                --Outstanding;
            }
        }
        CurrentWorker = nullptr;
    }

    inline static thread_local const Impl* CurrentWorker = nullptr;
    ResourceIoConfig Config;
    ReadFunction Reader;
    std::mutex Mutex;
    std::condition_variable Wakeup;
    std::deque<Job> Pending;
    std::size_t Outstanding = 0;
    bool Stopping = false;
    std::stop_source Cancellation;
    std::mutex JoinMutex;
    std::vector<std::thread> Workers;
};

ResourceIoRequest::ResourceIoRequest(std::shared_ptr<State> state)
    : m_state(std::move(state)) {
}

bool ResourceIoRequest::IsValid() const noexcept {
    return static_cast<bool>(m_state);
}

void ResourceIoRequest::Cancel() const {
    if (m_state) {
        m_state->Cancellation.request_stop();
    }
}

std::shared_ptr<const ResourceIoResult> ResourceIoRequest::GetResult() const {
    if (!m_state) {
        return {};
    }
    std::lock_guard lock{m_state->Mutex};
    return m_state->Published ? std::shared_ptr<const ResourceIoResult>{m_state, &m_state->Result} : nullptr;
}

std::optional<NCommon::ErrorInfo> ResourceIoRequest::GetCompletionFailure() const {
    if (!m_state) {
        return {};
    }
    std::lock_guard lock{m_state->Mutex};
    return m_state->CompletionFailure;
}

ResourceIo::ResourceIo(ResourceIoConfig config, ReadFunction reader)
    : m_impl(std::make_unique<Impl>(config, std::move(reader))) {
}

ResourceIo::~ResourceIo() = default;

ResourceIoRequest ResourceIo::Read(std::filesystem::path path, Completion completion) {
    if (path.empty()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "ResourceIo path must not be empty");
    }
    const auto state = std::make_shared<ResourceIoRequest::State>(std::move(path));
    Impl::Job job{state, std::move(completion)};
    {
        std::lock_guard lock{m_impl->Mutex};
        if (m_impl->Stopping || m_impl->Outstanding >= m_impl->Config.MaxPendingReads) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "ResourceIo is stopped or its queue is full");
        }
        m_impl->Pending.push_back(std::move(job));
        ++m_impl->Outstanding;
    }
    m_impl->Wakeup.notify_one();
    return ResourceIoRequest{state};
}

void ResourceIo::Shutdown() {
    m_impl->Shutdown();
}

} // namespace NResources
