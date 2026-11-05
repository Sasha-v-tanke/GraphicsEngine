#include "async_resource_loader.h"
#include "image_loader.h"
#include "mesh_loader.h"
#include "shader_artifact_loader.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

#include <GraphicsEngine/lib/common/error/exception.h>

namespace NResources {

struct AsyncResourceLoader::Work {
    OperationKey Key;
    ResourceIdentity Identity;
    std::filesystem::path Path;
    std::stop_source Stop;
    std::function<bool()> Current;
    std::function<void(NCommon::ErrorInfo)> Fail;
    std::function<void()> Cancel;
    std::function<bool()> Start;
    std::function<std::function<ResourceUpload()>(std::span<const std::byte>, std::stop_token)> Decode;
    std::function<void(std::shared_ptr<const void>)> Ready;
    std::mutex Mutex;
    ResourceIoRequest Io;
    NCommon::TaskHandle Task;
    std::function<ResourceUpload()> Submit;
    ResourceUpload Upload;
    std::atomic_bool CallbackDone = false;
    bool Decoded = false;
    bool Finished = false;

    void Failure(const std::exception& error) const {
        Fail({.Code = NCommon::EError::IO_ERROR,
              .Message =
                      "Resource '" + std::string{Identity.GetKey()} + "' at '" + Path.string() + "': " + error.what()});
    }
};

struct AsyncResourceLoader::Impl {
    NCommon::TaskSystem& Tasks;
    ResourceIo Io;
    std::thread::id Owner = std::this_thread::get_id();
    std::mutex Mutex;
    std::vector<std::shared_ptr<Work>> WorkItems;
    std::size_t Limit;
    bool Stopping = false;

    Impl(NCommon::TaskSystem& tasks, ResourceIoConfig config, ResourceIo::ReadFunction reader)
        : Tasks(tasks)
        , Io(config, std::move(reader))
        , Limit(config.MaxPendingReads) {
    }

    void CancelQueued(const NCommon::TaskHandle& task) {
        try {
            Tasks.Cancel(task);
        } catch (const NCommon::Exception& error) {
            // A queued task may start between observation and cancellation. Its stop_token handles that race.
            if (error.code() != NCommon::EError::INVALID_STATE) {
                throw;
            }
        }
    }

    void CheckOwner() const {
        if (std::this_thread::get_id() != Owner || NCommon::TaskSystem::IsCurrentThreadWorker()) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                                  "Resource upload pump/shutdown requires the host owner");
        }
    }
};

AsyncResourceLoader::AsyncResourceLoader(ResourceManager& resources,
                                         NCommon::TaskSystem& tasks,
                                         ResourceIoConfig config,
                                         ResourceIo::ReadFunction reader)
    : m_resources(resources)
    , m_impl(std::make_unique<Impl>(tasks, config, std::move(reader))) {
}

AsyncResourceLoader::~AsyncResourceLoader() noexcept {
    m_impl->CheckOwner();
    try {
        Shutdown();
    } catch (...) {
        // Backend drain errors are reported by explicit Shutdown. All tickets have already been drained.
    }
}

ResourceHandle<ImageData>
AsyncResourceLoader::LoadImage(ResourceIdentity identity, std::filesystem::path path, Uploader<ImageData> upload) {
    return Load<ImageData>(
            std::move(identity),
            std::move(path),
            [](auto bytes, const auto&) { return DecodeImage(bytes); },
            std::move(upload));
}

ResourceHandle<MeshData>
AsyncResourceLoader::LoadMesh(ResourceIdentity identity, std::filesystem::path path, Uploader<MeshData> upload) {
    return Load<MeshData>(
            std::move(identity),
            std::move(path),
            [](auto bytes, const auto&) { return DecodeMesh(bytes); },
            std::move(upload));
}

ResourceHandle<ShaderArtifact> AsyncResourceLoader::LoadShaderArtifact(ResourceIdentity identity,
                                                                       std::filesystem::path path,
                                                                       EShaderStage stage,
                                                                       Uploader<ShaderArtifact> upload) {
    return Load<ShaderArtifact>(
            std::move(identity),
            std::move(path),
            [stage](auto bytes, const auto&) { return DecodeShaderArtifact(bytes, stage); },
            std::move(upload));
}

void AsyncResourceLoader::Enqueue(
        OperationKey key,
        ResourceIdentity identity,
        std::filesystem::path path,
        std::function<bool()> current,
        std::function<void(NCommon::ErrorInfo)> fail,
        std::function<void()> cancel,
        std::function<bool()> start,
        std::function<std::function<ResourceUpload()>(std::span<const std::byte>, std::stop_token)> decode,
        std::function<void(std::shared_ptr<const void>)> ready) {
    auto work = std::make_shared<Work>();
    work->Key = key;
    work->Identity = std::move(identity);
    work->Path = std::move(path);
    work->Current = std::move(current);
    work->Fail = std::move(fail);
    work->Cancel = std::move(cancel);
    work->Start = std::move(start);
    work->Decode = std::move(decode);
    work->Ready = std::move(ready);
    // Keep admission and Read registration atomic with respect to Shutdown.
    std::unique_lock admission{m_impl->Mutex};
    if (m_impl->Stopping || m_impl->WorkItems.size() >= m_impl->Limit) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Resource pipeline stopped or full");
    }
    if (!work->Current()) {
        return;
    }
    m_impl->WorkItems.push_back(work);
    try {
        auto request = m_impl->Io.Read(work->Path, [this, work](const auto& result) {
            struct CompletionGuard {
                Work& Item;
                ~CompletionGuard() {
                    Item.CallbackDone.store(true);
                }
            } guard{*work};
            try {
                if (work->Stop.stop_requested() || !work->Current()) {
                    return;
                }
                if (result->Status != EResourceIoStatus::SUCCEEDED) {
                    if (result->Failure) {
                        work->Fail(*result->Failure);
                    } else {
                        work->Cancel();
                    }
                    return;
                }
                if (!work->Start()) {
                    return;
                }
                auto task = m_impl->Tasks.Submit([work, result](NCommon::TaskContext&) {
                    try {
                        if (work->Stop.stop_requested() || !work->Current()) {
                            return;
                        }
                        auto submit = work->Decode(*result->Bytes, work->Stop.get_token());
                        std::lock_guard lock{work->Mutex};
                        work->Submit = std::move(submit);
                        work->Decoded = true;
                    } catch (const std::exception& error) {
                        work->Failure(error);
                    } catch (...) {
                        work->Fail({.Code = NCommon::EError::IO_ERROR, .Message = "Unknown resource decode failure"});
                    }
                });
                std::lock_guard lock{work->Mutex};
                work->Task = std::move(task);
            } catch (const std::exception& error) {
                work->Failure(error);
            }
        });
        std::lock_guard lock{work->Mutex};
        work->Io = std::move(request);
    } catch (...) {
        m_impl->WorkItems.pop_back();
        throw;
    }
}

void AsyncResourceLoader::CancelMatching(OperationKey key) {
    std::vector<std::shared_ptr<Work>> items;
    {
        std::lock_guard lock{m_impl->Mutex};
        items = m_impl->WorkItems;
    }
    for (const auto& work: items) {
        if (work->Key == key) {
            work->Cancel();
            work->Stop.request_stop();
            ResourceIoRequest request;
            NCommon::TaskHandle task;
            {
                std::lock_guard lock{work->Mutex};
                request = work->Io;
                task = work->Task;
            }
            request.Cancel();
            if (task.IsValid()) {
                m_impl->CancelQueued(task);
            }
        }
    }
}

void AsyncResourceLoader::PumpUploads() {
    m_impl->CheckOwner();
    std::vector<std::shared_ptr<Work>> items;
    {
        std::lock_guard lock{m_impl->Mutex};
        items = m_impl->WorkItems;
    }
    for (const auto& work: items) {
        if (work->Finished) {
            continue;
        }
        try {
            std::function<ResourceUpload()> submit;
            {
                std::lock_guard lock{work->Mutex};
                if (work->Decoded) {
                    submit.swap(work->Submit);
                    work->Decoded = false;
                }
            }
            if (submit && work->Current() && !work->Stop.stop_requested()) {
                work->Upload = submit();
                if (work->Upload.Representation && (!work->Upload.IsComplete || !work->Upload.Wait)) {
                    GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                                          "Upload requires exact completion and drain");
                }
            }
            if (work->Upload.Representation) {
                if (!work->Upload.IsComplete()) {
                    continue;
                }
                work->Ready(work->Upload.Representation);
                work->Upload = {};
            }
        } catch (const std::exception& error) {
            work->Failure(error);
        } catch (...) {
            work->Fail({.Code = NCommon::EError::IO_ERROR, .Message = "Unknown resource upload failure"});
        }
        if (!work->Upload.Representation && !work->Current()) {
            // Even submit failures must wait for handoff/callable retirement before removing the work.
            std::lock_guard lock{work->Mutex};
            bool taskDone = !work->Task.IsValid();
            if (!taskDone) {
                const auto status = m_impl->Tasks.GetStatus(work->Task);
                taskDone = status == NCommon::ETaskStatus::COMPLETED || status == NCommon::ETaskStatus::FAILED ||
                           status == NCommon::ETaskStatus::CANCELLED;
            }
            work->Finished = work->CallbackDone.load() && taskDone;
        }
    }
    std::vector<std::shared_ptr<Work>> retired;
    {
        std::lock_guard lock{m_impl->Mutex};
        for (auto it = m_impl->WorkItems.begin(); it != m_impl->WorkItems.end();) {
            if ((*it)->Finished) {
                retired.push_back(std::move(*it));
                it = m_impl->WorkItems.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void AsyncResourceLoader::Shutdown() {
    m_impl->CheckOwner();
    std::vector<std::shared_ptr<Work>> items;
    {
        std::lock_guard lock{m_impl->Mutex};
        m_impl->Stopping = true;
        items.swap(m_impl->WorkItems);
    }
    for (const auto& work: items) {
        work->Cancel();
        work->Stop.request_stop();
    }
    m_impl->Io.Shutdown();
    std::exception_ptr failure;
    for (const auto& work: items) {
        if (work->Task.IsValid()) {
            m_impl->CancelQueued(work->Task);
            m_impl->Tasks.Wait(work->Task);
        }
        if (work->Upload.Representation && work->Upload.Wait) {
            try {
                work->Upload.Wait();
            } catch (...) {
                if (!failure) {
                    failure = std::current_exception();
                }
            }
        }
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}

} // namespace NResources
