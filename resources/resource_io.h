#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

#include <GraphicsEngine/lib/common/error/error.h>
#include <GraphicsEngine/lib/common/wrapper/non_transferable.h>

namespace NResources {

using ResourceIoBytes = std::vector<std::byte>;

enum class EResourceIoStatus {
    SUCCEEDED,
    FAILED,
    CANCELLED,
};

struct ResourceIoResult {
    std::filesystem::path Path;
    EResourceIoStatus Status = EResourceIoStatus::FAILED;
    std::shared_ptr<const ResourceIoBytes> Bytes;
    std::optional<NCommon::ErrorInfo> Failure;
};

// A retained operation, independent of the service lifetime. Observation never waits for I/O.
class ResourceIoRequest final {
public:
    ResourceIoRequest() = default;

    [[nodiscard]] bool IsValid() const noexcept;
    void Cancel() const;
    [[nodiscard]] std::shared_ptr<const ResourceIoResult> GetResult() const;
    // Separate from the read result: a throwing completion cannot revoke already delivered bytes.
    [[nodiscard]] std::optional<NCommon::ErrorInfo> GetCompletionFailure() const;

private:
    struct State;
    explicit ResourceIoRequest(std::shared_ptr<State> state);

    std::shared_ptr<State> m_state;
    friend class ResourceIo;
};

struct ResourceIoConfig {
    std::size_t WorkerCount = 2;
    // Includes active reads and completion callbacks, not just queued requests.
    std::size_t MaxPendingReads = 64;
    std::size_t MaxReadBytes = 64 * 1024 * 1024;
};

class ResourceIo final: private NCommon::NonTransferable {
public:
    using Completion = std::function<void(std::shared_ptr<const ResourceIoResult>)>;
    // Optional I/O backend; runs only on dedicated I/O threads. It must support concurrent invocation,
    // honor cancellation at safe points and return at most maxReadBytes. An empty function uses filesystem I/O.
    using ReadFunction =
            std::function<ResourceIoBytes(const std::filesystem::path&, std::stop_token, std::size_t maxReadBytes)>;

    explicit ResourceIo(ResourceIoConfig config = {}, ReadFunction reader = {});
    ~ResourceIo();

    // Admission is non-blocking. Full/stopped service throws INVALID_STATE, invalid path INVALID_ARGUMENT.
    // Completion runs exactly once on an I/O thread, outside service locks, after result publication.
    // It should only hand off bytes (e.g. TaskSystem::Submit), never decode, wait or destroy/stop this service.
    [[nodiscard]] ResourceIoRequest Read(std::filesystem::path path, Completion completion = {});

    // Cancels accepted work, drains completions and joins threads. Idempotent; use from the owner thread
    // before stopping TaskSystem. Cancellation cannot interrupt an in-progress operating-system call.
    void Shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace NResources
