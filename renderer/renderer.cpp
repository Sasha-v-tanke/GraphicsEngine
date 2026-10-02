#include "renderer.h"

#include <algorithm>
#include <memory_resource>
#include <new>
#include <type_traits>
#include <vector>

namespace NRenderer {

namespace {

struct RenderItem {
    const RenderView* View = nullptr;
    const RenderObject* Object = nullptr;
};

[[nodiscard]] bool LessRenderObjectId(RenderObjectId left, RenderObjectId right) noexcept {
    if (left.Index != right.Index) {
        return left.Index < right.Index;
    }

    return left.Generation < right.Generation;
}

template<typename T>
[[nodiscard]] bool LessResource(NResources::ResourceHandle<T> left, NResources::ResourceHandle<T> right) noexcept {
    if (left.GetSlotIndex() != right.GetSlotIndex()) {
        return left.GetSlotIndex() < right.GetSlotIndex();
    }

    return left.GetGeneration() < right.GetGeneration();
}

[[nodiscard]] bool LessRenderItem(const RenderItem& left, const RenderItem& right) noexcept {
    const auto& leftObject = *left.Object;
    const auto& rightObject = *right.Object;

    if (!LessRenderObjectId(left.View->Id, right.View->Id) && !LessRenderObjectId(right.View->Id, left.View->Id)) {
        if (LessResource(leftObject.Material.GetResource(), rightObject.Material.GetResource())) {
            return true;
        }

        if (LessResource(rightObject.Material.GetResource(), leftObject.Material.GetResource())) {
            return false;
        }

        if (leftObject.Material.GetVersion() != rightObject.Material.GetVersion()) {
            return leftObject.Material.GetVersion() < rightObject.Material.GetVersion();
        }

        if (LessResource(leftObject.Mesh.GetResource(), rightObject.Mesh.GetResource())) {
            return true;
        }

        if (LessResource(rightObject.Mesh.GetResource(), leftObject.Mesh.GetResource())) {
            return false;
        }

        if (leftObject.Mesh.GetVersion() != rightObject.Mesh.GetVersion()) {
            return leftObject.Mesh.GetVersion() < rightObject.Mesh.GetVersion();
        }

        return LessRenderObjectId(leftObject.Id, rightObject.Id);
    }

    return LessRenderObjectId(left.View->Id, right.View->Id);
}

[[nodiscard]] DrawCommandData MakeDrawCommand(const RenderItem& item) noexcept {
    return {
            .ViewId = item.View->Id,
            .ObjectId = item.Object->Id,
            .WorldTransform = item.Object->WorldTransform,
            .Mesh = item.Object->Mesh.GetResource(),
            .MeshVersion = item.Object->Mesh.GetVersion(),
            .Material = item.Object->Material.GetResource(),
            .MaterialVersion = item.Object->Material.GetVersion(),
    };
}

} // namespace

const RenderPlan& Renderer::Prepare(const RenderWorld& world, std::pmr::memory_resource& memory) {
    static_assert(std::is_trivially_destructible_v<DrawCommandData>);
    static_assert(std::is_trivially_destructible_v<RenderPlan>);

    std::pmr::vector<RenderItem> visibleItems{&memory};
    visibleItems.reserve(world.GetViews().size() * world.GetObjects().size());

    for (const RenderView& view: world.GetViews()) {
        for (const RenderObject& object: world.GetObjects()) {
            visibleItems.push_back({
                    .View = &view,
                    .Object = &object,
            });
        }
    }

    std::ranges::stable_sort(visibleItems, LessRenderItem);

    DrawCommandData* commands = nullptr;

    if (!visibleItems.empty()) {
        void* commandStorage = memory.allocate(sizeof(DrawCommandData) * visibleItems.size(), alignof(DrawCommandData));
        commands = static_cast<DrawCommandData*>(commandStorage);

        for (std::size_t index = 0; index < visibleItems.size(); ++index) {
            std::construct_at(commands + index, MakeDrawCommand(visibleItems[index]));
        }
    }

    void* planStorage = memory.allocate(sizeof(RenderPlan), alignof(RenderPlan));

    return *::new (planStorage) RenderPlan{world.GetFrame(), commands, visibleItems.size()};
}

} // namespace NRenderer
