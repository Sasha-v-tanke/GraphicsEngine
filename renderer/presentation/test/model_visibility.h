#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <vulkan.h>

namespace NModelTest {

// Test-only real-driver probe. Split the cube's original draw into front/back/rest
// without changing its triangles, and measure rasterized samples with GPU occlusion queries.
class VisibilityProbe {
public:
    void Install() {
        if (Active != nullptr) {
            throw std::runtime_error{"Visibility probe already active"};
        }
        Active = this;
        OriginalCreate = vkCreateGraphicsPipelines;
        OriginalBegin = vkBeginCommandBuffer;
        OriginalDraw = vkCmdDrawIndexed;
        OriginalPresent = vkQueuePresentKHR;
        OriginalDestroy = vkDestroyDevice;
        vkCreateGraphicsPipelines = Create;
        vkBeginCommandBuffer = Begin;
        vkCmdDrawIndexed = Draw;
        vkQueuePresentKHR = Present;
        vkDestroyDevice = Destroy;
    }

    ~VisibilityProbe() {
        if (Active == this) {
            Restore();
        }
    }

    std::array<std::uint64_t, 2> Samples{};
    bool FlipFrontFace = false;
    bool Observed = false;
    bool Failed = false;

private:
    void Restore() {
        vkCreateGraphicsPipelines = OriginalCreate;
        vkBeginCommandBuffer = OriginalBegin;
        vkCmdDrawIndexed = OriginalDraw;
        vkQueuePresentKHR = OriginalPresent;
        vkDestroyDevice = OriginalDestroy;
        Active = nullptr;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL Create(VkDevice device,
                                                 VkPipelineCache cache,
                                                 std::uint32_t count,
                                                 const VkGraphicsPipelineCreateInfo* info,
                                                 const VkAllocationCallbacks* allocator,
                                                 VkPipeline* pipelines) {
        VkGraphicsPipelineCreateInfo changed{};
        VkPipelineRasterizationStateCreateInfo raster{};
        if (Active->FlipFrontFace && count == 1) {
            changed = info[0];
            raster = *changed.pRasterizationState;
            raster.frontFace = raster.frontFace == VK_FRONT_FACE_COUNTER_CLOCKWISE ? VK_FRONT_FACE_CLOCKWISE
                                                                                   : VK_FRONT_FACE_COUNTER_CLOCKWISE;
            changed.pRasterizationState = &raster;
            info = &changed;
        }
        const VkResult result = Active->OriginalCreate(device, cache, count, info, allocator, pipelines);
        if (result == VK_SUCCESS && Active->Pool == VK_NULL_HANDLE) {
            const VkQueryPoolCreateInfo query{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                              .pNext = nullptr,
                                              .flags = 0,
                                              .queryType = VK_QUERY_TYPE_OCCLUSION,
                                              .queryCount = 2,
                                              .pipelineStatistics = 0};
            Active->Device = device;
            Active->Failed = vkCreateQueryPool(device, &query, nullptr, &Active->Pool) != VK_SUCCESS;
        }
        return result;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL Begin(VkCommandBuffer command, const VkCommandBufferBeginInfo* info) {
        const VkResult result = Active->OriginalBegin(command, info);
        if (result == VK_SUCCESS && Active->Pool != VK_NULL_HANDLE) {
            vkCmdResetQueryPool(command, Active->Pool, 0, 2);
        }
        return result;
    }

    static VKAPI_ATTR void VKAPI_CALL Draw(VkCommandBuffer command,
                                           std::uint32_t indexCount,
                                           std::uint32_t instances,
                                           std::uint32_t firstIndex,
                                           std::int32_t vertexOffset,
                                           std::uint32_t firstInstance) {
        if (Active->Pool == VK_NULL_HANDLE || indexCount != 36 || firstIndex != 0) {
            Active->Failed = true;
            Active->OriginalDraw(command, indexCount, instances, firstIndex, vertexOffset, firstInstance);
            return;
        }
        for (std::uint32_t face = 0; face < 2; ++face) {
            vkCmdBeginQuery(command, Active->Pool, face, 0);
            Active->OriginalDraw(command, 6, instances, face * 6, vertexOffset, firstInstance);
            vkCmdEndQuery(command, Active->Pool, face);
        }
        Active->OriginalDraw(command, 24, instances, 12, vertexOffset, firstInstance);
    }

    static VKAPI_ATTR VkResult VKAPI_CALL Present(VkQueue queue, const VkPresentInfoKHR* info) {
        const VkResult result = Active->OriginalPresent(queue, info);
        if (Active->Pool != VK_NULL_HANDLE) {
            const VkResult query = vkGetQueryPoolResults(Active->Device,
                                                         Active->Pool,
                                                         0,
                                                         2,
                                                         sizeof(Active->Samples),
                                                         Active->Samples.data(),
                                                         sizeof(std::uint64_t),
                                                         VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            Active->Failed = Active->Failed || query != VK_SUCCESS;
            Active->Observed = true;
        }
        return result;
    }

    static VKAPI_ATTR void VKAPI_CALL Destroy(VkDevice device, const VkAllocationCallbacks* allocator) {
        // GraphPresentation has already drained the submitted query consumers.
        if (Active->Pool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(device, Active->Pool, nullptr);
            Active->Pool = VK_NULL_HANDLE;
        }
        const PFN_vkDestroyDevice original = Active->OriginalDestroy;
        Active->Restore();
        original(device, allocator);
    }

    inline static VisibilityProbe* Active = nullptr;
    VkDevice Device = VK_NULL_HANDLE;
    VkQueryPool Pool = VK_NULL_HANDLE;
    PFN_vkCreateGraphicsPipelines OriginalCreate = nullptr;
    PFN_vkBeginCommandBuffer OriginalBegin = nullptr;
    PFN_vkCmdDrawIndexed OriginalDraw = nullptr;
    PFN_vkQueuePresentKHR OriginalPresent = nullptr;
    PFN_vkDestroyDevice OriginalDestroy = nullptr;
};

} // namespace NModelTest
