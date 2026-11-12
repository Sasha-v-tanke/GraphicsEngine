#include "frame_context.h"

#include <exception>
#include <limits>
#include <string_view>

#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NVulkan {

namespace {

std::string_view GetVkResultName(VkResult result) {
    switch (result) {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_DEVICE_LOST:
        return "VK_ERROR_DEVICE_LOST";
    default:
        return "VK_RESULT_UNKNOWN";
    }
}

[[nodiscard]] VkCommandPool CreateCommandPool(VkDevice device, std::uint32_t queueFamilyIndex) {
    const VkCommandPoolCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = queueFamilyIndex,
    };

    VkCommandPool commandPool = VK_NULL_HANDLE;
    const VkResult result = vkCreateCommandPool(device, &createInfo, nullptr, &commandPool);

    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to create Vulkan frame command pool: {}",
                              GetVkResultName(result));
    }

    return commandPool;
}

void ResetCommandPool(VkDevice device, VkCommandPool commandPool) {
    const VkResult result = vkResetCommandPool(device, commandPool, 0);

    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to reset Vulkan frame command pool: {}",
                              GetVkResultName(result));
    }
}

} // namespace

struct VulkanFrameContext::WorkerState: NCommon::NonTransferable {
    WorkerState(VkDevice device, std::uint32_t queueFamilyIndex)
        : Device(device)
        , Pool(CreateCommandPool(device, queueFamilyIndex)) {
    }
    ~WorkerState() {
        vkDestroyCommandPool(Device, Pool, nullptr);
    }
    VkDevice Device;
    VkCommandPool Pool;
    bool Active = false;
};

VulkanWorkerContext::VulkanWorkerContext(const NCommon::TaskSystem& system,
                                         const NCommon::TaskContext& task,
                                         VkCommandPool pool,
                                         VulkanFrameContextAcquire frame) noexcept
    : m_system(system)
    , m_task(task)
    , m_pool(pool)
    , m_frame(frame) {
}

VkCommandPool VulkanWorkerContext::GetCommandPool() const {
    if (!m_task.IsCurrentInvocation(m_system)) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Vulkan worker context is outside its task invocation");
    }
    return m_pool;
}

NCommon::WorkerIndex VulkanWorkerContext::GetWorkerIndex() const noexcept {
    return m_task.GetWorkerIndex();
}

std::uint64_t VulkanWorkerContext::GetFrameIndex() const noexcept {
    return m_frame.FrameIndex;
}

std::size_t VulkanWorkerContext::GetFrameSlotIndex() const noexcept {
    return m_frame.FrameSlotIndex;
}

VulkanFrameContext::VulkanFrameContext(VkDevice device,
                                       std::size_t frameSlotIndex,
                                       std::uint32_t queueFamilyIndex,
                                       const NCommon::TaskSystem* tasks)
    : m_tasks(tasks)
    , m_device(device)
    , m_frameSlotIndex(frameSlotIndex)
    , m_commandPool(CreateCommandPool(device, queueFamilyIndex)) {
    try {
        const std::size_t workers = tasks != nullptr ? tasks->GetWorkerCount() : 0;
        m_workers.reserve(workers);
        for (std::size_t index = 0; index < workers; ++index) {
            m_workers.push_back(std::make_unique<WorkerState>(device, queueFamilyIndex));
        }
    } catch (...) {
        vkDestroyCommandPool(m_device, m_commandPool, nullptr);
        throw;
    }
}

VulkanFrameContext::~VulkanFrameContext() {
    const std::lock_guard lock{m_mutex};
    if (m_activeWorkers != 0) {
        std::terminate();
    }
    if (m_commandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_device, m_commandPool, nullptr);
        m_commandPool = VK_NULL_HANDLE;
    }
}

std::uint64_t VulkanFrameContext::GetFrameIndex() const noexcept {
    const std::lock_guard lock{m_mutex};
    return m_frameIndex;
}

std::size_t VulkanFrameContext::GetFrameSlotIndex() const noexcept {
    return m_frameSlotIndex;
}

VkCommandPool VulkanFrameContext::GetCommandPool() const noexcept {
    return m_commandPool;
}

std::optional<std::uint64_t> VulkanFrameContext::GetCompletionValue() const noexcept {
    const std::lock_guard lock{m_mutex};
    return m_completionValue;
}

EVulkanFrameContextState VulkanFrameContext::GetState() const noexcept {
    const std::lock_guard lock{m_mutex};
    return m_state;
}

void VulkanFrameContext::Acquire(std::uint64_t frameIndex) {
    const std::lock_guard lock{m_mutex};
    if (m_state != EVulkanFrameContextState::FREE) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Vulkan frame context {} cannot be acquired from state {}",
                              m_frameSlotIndex,
                              static_cast<int>(m_state));
    }

    ResetCommandPool(m_device, m_commandPool);
    for (const auto& worker: m_workers) {
        ResetCommandPool(m_device, worker->Pool);
    }
    m_frameIndex = frameIndex;
    m_state = EVulkanFrameContextState::ACQUIRED;
}

void VulkanFrameContext::MarkSubmitted(std::uint64_t completionValue) {
    const std::lock_guard lock{m_mutex};
    if (m_state != EVulkanFrameContextState::ACQUIRED || m_activeWorkers != 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Vulkan frame context {} cannot be submitted from state {}",
                              m_frameSlotIndex,
                              static_cast<int>(m_state));
    }

    if (completionValue == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan frame completion value must be valid");
    }

    m_completionValue = completionValue;
    m_state = EVulkanFrameContextState::IN_FLIGHT;
}

void VulkanFrameContext::ReleaseCompleted() noexcept {
    const std::lock_guard lock{m_mutex};
    m_completionValue.reset();
    m_state = EVulkanFrameContextState::FREE;
}

bool VulkanFrameContext::IsInFlight() const noexcept {
    const std::lock_guard lock{m_mutex};
    return m_state == EVulkanFrameContextState::IN_FLIGHT;
}

bool VulkanFrameContext::MatchesFrame(std::uint64_t frameIndex) const noexcept {
    const std::lock_guard lock{m_mutex};
    return m_state != EVulkanFrameContextState::FREE && m_frameIndex == frameIndex;
}

std::size_t VulkanFrameContext::GetWorkerCount() const noexcept {
    return m_workers.size();
}

void VulkanFrameContext::WithWorkerContext(std::uint64_t frameIndex,
                                           NCommon::TaskContext& task,
                                           const std::function<void(VulkanWorkerContext&)>& record) {
    const std::size_t index = task.GetWorkerIndex().GetValue();
    {
        const std::lock_guard lock{m_mutex};
        if (m_tasks == nullptr || !task.IsCurrentInvocation(*m_tasks) || index >= m_workers.size()) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                                  "Vulkan worker context requires the configured scheduler's current invocation");
        }
        if (m_state != EVulkanFrameContextState::ACQUIRED || m_frameIndex != frameIndex || m_workers[index]->Active) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                                  "Vulkan worker context is already borrowed or its frame is not acquired");
        }
        m_workers[index]->Active = true;
        ++m_activeWorkers;
    }
    struct ReturnWorker {
        std::mutex& Mutex;
        bool& Active;
        std::size_t& Count;
        ~ReturnWorker() {
            const std::lock_guard lock{Mutex};
            Active = false;
            --Count;
        }
    } release{m_mutex, m_workers[index]->Active, m_activeWorkers};
    VulkanWorkerContext context{*m_tasks, task, m_workers[index]->Pool, {frameIndex, m_frameSlotIndex}};
    record(context);
}

VulkanFrameContextRing::VulkanFrameContextRing(const VulkanDevice& device, std::size_t maxActiveFrames)
    : VulkanFrameContextRing(device, maxActiveFrames, nullptr) {
}

VulkanFrameContextRing::VulkanFrameContextRing(const VulkanDevice& device,
                                               std::size_t maxActiveFrames,
                                               const NCommon::TaskSystem& tasks)
    : VulkanFrameContextRing(device, maxActiveFrames, &tasks) {
}

VulkanFrameContextRing::VulkanFrameContextRing(const VulkanDevice& device,
                                               std::size_t maxActiveFrames,
                                               const NCommon::TaskSystem* tasks)
    : m_device(device.GetHandle()) {
    if (m_device == VK_NULL_HANDLE) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan frame context ring requires a device");
    }

    if (maxActiveFrames == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                              "Vulkan frame context ring size must be greater than zero");
    }

    VulkanLockedQueue graphicsQueue = device.LockGraphicsQueue();
    m_contexts.reserve(maxActiveFrames);

    for (std::size_t frameSlotIndex = 0; frameSlotIndex < maxActiveFrames; ++frameSlotIndex) {
        m_contexts.emplace_back(std::unique_ptr<VulkanFrameContext>{
                new VulkanFrameContext{m_device, frameSlotIndex, graphicsQueue.GetFamilyIndex(), tasks}});
    }
}

VulkanFrameContextRing::~VulkanFrameContextRing() {
    for (const std::unique_ptr<VulkanFrameContext>& context: m_contexts) {
        if (context != nullptr && context->IsInFlight()) {
            vkDeviceWaitIdle(m_device);
            break;
        }
    }
}

std::size_t VulkanFrameContextRing::GetSize() const noexcept {
    return m_contexts.size();
}

VulkanFrameContext* VulkanFrameContextRing::TryAcquire(VulkanFrameContextAcquire acquire,
                                                       const CompletionQuery& isCompleted) {
    VulkanFrameContext& context = GetBySlot(acquire.FrameSlotIndex);

    if (context.GetState() == EVulkanFrameContextState::ACQUIRED) {
        return nullptr;
    }

    if (context.GetState() == EVulkanFrameContextState::IN_FLIGHT) {
        if (!isCompleted || !isCompleted(*context.GetCompletionValue())) {
            return nullptr;
        }

        context.ReleaseCompleted();
    }

    context.Acquire(acquire.FrameIndex);
    return &context;
}

VulkanFrameContext& VulkanFrameContextRing::Get(VulkanFrameContextAcquire acquire) {
    VulkanFrameContext& context = GetBySlot(acquire.FrameSlotIndex);

    if (!context.MatchesFrame(acquire.FrameIndex)) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                              "Vulkan frame context {} does not match frame {}",
                              acquire.FrameSlotIndex,
                              acquire.FrameIndex);
    }

    return context;
}

void VulkanFrameContextRing::MarkSubmitted(VulkanFrameContextAcquire acquire, std::uint64_t completionValue) {
    Get(acquire).MarkSubmitted(completionValue);
}

void VulkanFrameContextRing::ReleaseAcquired(VulkanFrameContextAcquire acquire) {
    VulkanFrameContext& context = Get(acquire);
    const std::lock_guard lock{context.m_mutex};
    if (context.m_state != EVulkanFrameContextState::ACQUIRED || context.m_activeWorkers != 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Vulkan frame context {} cannot be released from state {}",
                              acquire.FrameSlotIndex,
                              static_cast<int>(context.m_state));
    }

    context.m_completionValue.reset();
    context.m_state = EVulkanFrameContextState::FREE;
}

void VulkanFrameContextRing::WithWorkerContext(VulkanFrameContextAcquire acquire,
                                               NCommon::TaskContext& task,
                                               const std::function<void(VulkanWorkerContext&)>& record) {
    GetBySlot(acquire.FrameSlotIndex).WithWorkerContext(acquire.FrameIndex, task, record);
}

VulkanFrameContext& VulkanFrameContextRing::GetBySlot(std::size_t frameSlotIndex) {
    if (frameSlotIndex >= m_contexts.size()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                              "Vulkan frame slot {} is out of range [0, {})",
                              frameSlotIndex,
                              m_contexts.size());
    }

    return *m_contexts[frameSlotIndex];
}

} // namespace NVulkan
