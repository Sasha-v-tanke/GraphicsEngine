#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <stop_token>

#include <GraphicsEngine/lib/thread/task/task_system.h>
#include <GraphicsEngine/resources/image_data.h>
#include <GraphicsEngine/resources/mesh_data.h>
#include <GraphicsEngine/resources/resource_io.h>
#include <GraphicsEngine/resources/resource_manager.h>
#include <GraphicsEngine/resources/shader_artifact.h>

namespace NResources {

// Backend-owned representation and exact submission completion. Poll never waits.
// Wait is used only during host shutdown; it must retire this submission even after cancellation.
// The backend must support representation release on any lease-holder thread, or retain it in its owner GC.
struct ResourceUpload {
    std::shared_ptr<const void> Representation;
    std::function<bool()> IsComplete;
    std::function<void()> Wait;
};

class AsyncResourceLoader final: private NCommon::NonTransferable {
public:
    template<typename T>
    using Decoder = std::function<std::shared_ptr<const T>(std::span<const std::byte>, std::stop_token)>;
    template<typename T>
    using Uploader = std::function<ResourceUpload(std::shared_ptr<const T>)>;

    // Manager and executor outlive this service. Destruction/Shutdown/PumpUploads run on the constructor thread.
    explicit AsyncResourceLoader(ResourceManager& resources,
                                 NCommon::TaskSystem& tasks,
                                 ResourceIoConfig config = {},
                                 ResourceIo::ReadFunction reader = {});
    ~AsyncResourceLoader();

    // Nonblocking admission, safe for concurrent callers. Same identity/type implies the same source/decoder.
    // The first admitted request wins; errors become FAILED and may be retried by another Load.
    template<typename T>
    ResourceHandle<T>
    Load(ResourceIdentity identity, std::filesystem::path path, Decoder<T> decode, Uploader<T> upload = {}) {
        const auto handle = m_resources.Request<T>(std::move(identity));
        const auto operation = m_resources.TryQueueLoading(handle);
        if (!operation) {
            return handle;
        }
        try {
            Enqueue(
                    m_resources.GetIdentity(handle),
                    std::move(path),
                    [this, op = *operation] { return m_resources.IsOperationCurrent(op); },
                    [this, op = *operation](NCommon::ErrorInfo error) { m_resources.TryFail(op, std::move(error)); },
                    [this, op = *operation] { m_resources.TryCancel(op); },
                    [this, op = *operation] {
                        return m_resources.TryAdvance(op, EResourceState::QUEUED, EResourceState::LOADING);
                    },
                    [this, op = *operation, decode = std::move(decode), upload = std::move(upload)](
                            std::span<const std::byte> bytes,
                            const std::stop_token& stop) -> std::function<ResourceUpload()> {
                        auto cpu = decode(bytes, stop);
                        if (!cpu) {
                            throw std::invalid_argument("Resource decoder returned null");
                        }
                        if (stop.stop_requested() ||
                            !m_resources.TryAdvance(op, EResourceState::LOADING, EResourceState::CPU_READY, cpu)) {
                            return {};
                        }
                        return [this, op, cpu = std::move(cpu), upload]() {
                            if (!upload) {
                                m_resources.TryAdvance(op, EResourceState::CPU_READY, EResourceState::READY);
                                return ResourceUpload{};
                            }
                            if (!m_resources.TryAdvance(op, EResourceState::CPU_READY, EResourceState::UPLOADING)) {
                                return ResourceUpload{};
                            }
                            auto result = upload(cpu);
                            if (!result.Representation || !result.IsComplete || !result.Wait) {
                                throw std::invalid_argument("Upload requires representation, poll and drain");
                            }
                            return result;
                        };
                    },
                    [this, op = *operation](std::shared_ptr<const void> gpu) {
                        if (!m_resources.IsOperationCurrent(op)) {
                            return;
                        }
                        auto cpu = m_resources.GetCpuResource(op.GetResource());
                        m_resources.TryAdvance(op,
                                               EResourceState::UPLOADING,
                                               EResourceState::READY,
                                               std::move(cpu),
                                               std::move(gpu));
                    });
        } catch (const std::exception& error) {
            m_resources.TryFail(*operation, {.Code = NCommon::EError::IO_ERROR, .Message = error.what()});
        }
        return handle;
    }

    ResourceHandle<ImageData>
    LoadImage(ResourceIdentity identity, std::filesystem::path path, Uploader<ImageData> upload = {});
    ResourceHandle<MeshData>
    LoadMesh(ResourceIdentity identity, std::filesystem::path path, Uploader<MeshData> upload = {});
    ResourceHandle<ShaderArtifact> LoadShaderArtifact(ResourceIdentity identity,
                                                      std::filesystem::path path,
                                                      EShaderStage stage,
                                                      Uploader<ShaderArtifact> upload = {});

    // Cancellation is generation-specific and permits immediate retry. Already submitted GPU work is retained.
    template<typename T>
    void Cancel(ResourceHandle<T> handle) {
        CancelMatching(m_resources.GetIdentity(handle));
    }

    // Host/backend owner tick: submits CPU_READY work and polls GPU completion; never waits.
    void PumpUploads();
    // Host only: cancels I/O/decode, joins owned tasks, drains exact GPU completions. Call before executor shutdown.
    void Shutdown();

private:
    struct Work;
    struct Impl;
    void Enqueue(ResourceIdentity identity,
                 std::filesystem::path path,
                 std::function<bool()> current,
                 std::function<void(NCommon::ErrorInfo)> fail,
                 std::function<void()> cancel,
                 std::function<bool()> start,
                 std::function<std::function<ResourceUpload()>(std::span<const std::byte>, std::stop_token)> decode,
                 std::function<void(std::shared_ptr<const void>)> ready);
    void CancelMatching(const ResourceIdentity& identity);
    ResourceManager& m_resources;
    std::unique_ptr<Impl> m_impl;
};

} // namespace NResources
