#pragma once

#include <memory>

#include <engine/runtime/update_completion.h>

namespace NEcs {
class World;
}
namespace NCommon {
class TaskSystem;
}
namespace NEngine::NRuntime {
// Scene callers pass an aliasing shared_ptr<World> retaining the acquired Scene owner.
void ScheduleWorldUpdate(std::shared_ptr<NEcs::World> world,
                         NCommon::TaskSystem& tasks,
                         const std::shared_ptr<UpdateCompletion>& completion);
} // namespace NEngine::NRuntime
