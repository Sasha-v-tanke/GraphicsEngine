#include "renderer.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <GraphicsEngine/lib/common/error/error.h>
#include <GraphicsEngine/lib/common/error/exception.h>

namespace NRenderer {

namespace {

[[nodiscard]] auto BatchKey(const DrawCommandData& command) noexcept {
    return std::tuple{command.ViewId.Index,
                      command.ViewId.Generation,
                      command.Material.GetSlotIndex(),
                      command.Material.GetGeneration(),
                      command.MaterialVersion,
                      command.Mesh.GetSlotIndex(),
                      command.Mesh.GetGeneration(),
                      command.MeshVersion};
}

[[nodiscard]] bool LessCommand(const DrawCommandData& left, const DrawCommandData& right) noexcept {
    if (BatchKey(left) != BatchKey(right)) {
        return BatchKey(left) < BatchKey(right);
    }
    return std::tie(left.ObjectId.Index, left.ObjectId.Generation) <
           std::tie(right.ObjectId.Index, right.ObjectId.Generation);
}

[[nodiscard]] std::size_t ItemCount(const RenderWorld& world) {
    const auto views = world.GetViews().size();
    const auto objects = world.GetObjects().size();
    if (objects != 0 && views > std::numeric_limits<std::size_t>::max() / objects) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Renderer item count overflow");
    }
    return views * objects;
}

[[nodiscard]] DrawCommandData PrepareItem(const RenderWorld& world, std::size_t index) noexcept {
    const auto& view = world.GetViews()[index / world.GetObjects().size()];
    const auto& object = world.GetObjects()[index % world.GetObjects().size()];
    return {view.Id,
            object.Id,
            object.WorldTransform,
            object.Mesh.GetResource(),
            object.Mesh.GetVersion(),
            object.Material.GetResource(),
            object.Material.GetVersion()};
}

[[nodiscard]] DrawCommandData* AllocateCommands(std::size_t count, std::pmr::memory_resource& memory) {
    return count == 0 ? nullptr : std::pmr::polymorphic_allocator<DrawCommandData>{&memory}.allocate(count);
}

} // namespace

const RenderPlan& Renderer::Publish(RenderFrameIdentity frame,
                                    std::span<const DrawCommandData> sortedCommands,
                                    std::pmr::memory_resource& memory) {
    static_assert(std::is_trivially_destructible_v<DrawCommandData>);
    static_assert(std::is_trivially_destructible_v<RenderBatch>);
    static_assert(std::is_trivially_destructible_v<RenderPlan>);

    // Sorting establishes order; batching separately groups compatible resource versions within that order.
    std::pmr::vector<RenderBatch> batches{&memory};
    for (std::size_t index = 0; index < sortedCommands.size(); ++index) {
        if (index == 0 || BatchKey(sortedCommands[index - 1]) != BatchKey(sortedCommands[index])) {
            batches.push_back({index, 1});
        } else {
            ++batches.back().CommandCount;
        }
    }
    RenderBatch* publishedBatches = nullptr;
    if (!batches.empty()) {
        std::pmr::polymorphic_allocator<RenderBatch> batchAllocator{&memory};
        publishedBatches = batchAllocator.allocate(batches.size());
        std::uninitialized_copy(batches.begin(), batches.end(), publishedBatches);
    }
    void* planStorage = memory.allocate(sizeof(RenderPlan), alignof(RenderPlan));
    return *::new (planStorage) RenderPlan{frame, sortedCommands, {publishedBatches, batches.size()}};
}

const RenderPlan& Renderer::Prepare(const RenderWorld& world, std::pmr::memory_resource& memory) {
    const auto count = ItemCount(world);
    auto* commands = AllocateCommands(count, memory);
    for (std::size_t index = 0; index < count; ++index) {
        std::construct_at(commands + index, PrepareItem(world, index));
    }
    std::span items{commands, count};
    std::ranges::sort(items, LessCommand);
    return Publish(world.GetFrame(), items, memory);
}

NCommon::TaskHandle Renderer::SchedulePrepare(const RenderWorld& world,
                                              std::pmr::memory_resource& memory,
                                              NCommon::TaskContext& task,
                                              std::function<void(const RenderPlan&)> completion,
                                              std::size_t chunkSize) {
    if (chunkSize == 0 || !completion) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Renderer requires a chunk size and completion");
    }
    const auto count = ItemCount(world);
    const NCommon::TaskHandle gate[] = {task.GetTask()};
    if (count <= chunkSize) {
        return task.Spawn([&world, &memory, completion = std::move(completion)](
                                  NCommon::TaskContext&) { completion(Prepare(world, memory)); },
                          gate);
    }

    struct Chunk {
        std::vector<DrawCommandData> Items;
        std::exception_ptr Error;
    };
    const auto chunkCount = count / chunkSize + (count % chunkSize != 0 ? 1U : 0U);
    auto chunks = std::make_shared<std::vector<Chunk>>(chunkCount);
    std::vector<NCommon::TaskHandle> dependencies;
    dependencies.reserve(chunkCount);
    // Gating chunks on the caller also makes partial scheduling failure safe before any snapshot reads begin.
    for (std::size_t index = 0; index < chunkCount; ++index) {
        dependencies.push_back(task.Spawn(
                [&world, chunks, index, count, chunkSize](NCommon::TaskContext&) {
                    auto& chunk = (*chunks)[index];
                    try {
                        const auto begin = index * chunkSize;
                        const auto end = begin + std::min(chunkSize, count - begin);
                        chunk.Items.reserve(end - begin);
                        for (std::size_t item = begin; item < end; ++item) {
                            chunk.Items.push_back(PrepareItem(world, item));
                        }
                    } catch (...) {
                        // A failed chunk must not cancel the join while other snapshot readers are still running.
                        chunk.Error = std::current_exception();
                    }
                },
                gate));
    }
    return task.Spawn(
            [&world, &memory, chunks, count, completion = std::move(completion)](NCommon::TaskContext&) {
                for (const auto& chunk: *chunks) {
                    if (chunk.Error) {
                        std::rethrow_exception(chunk.Error);
                    }
                }
                // Only the join touches the caller's arena. Merge order and the final comparator ignore worker
                // completion.
                auto* commands = AllocateCommands(count, memory);
                std::size_t offset = 0;
                for (const auto& chunk: *chunks) {
                    std::uninitialized_copy(chunk.Items.begin(), chunk.Items.end(), commands + offset);
                    offset += chunk.Items.size();
                }
                std::span items{commands, count};
                std::ranges::sort(items, LessCommand);
                completion(Publish(world.GetFrame(), items, memory));
            },
            dependencies);
}

} // namespace NRenderer
