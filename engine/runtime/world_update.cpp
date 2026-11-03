#include "world_update.h"

#include <utility>

#include <ecs/internal/system_run.h>

namespace NEngine::NRuntime {
void ScheduleWorldUpdate(std::shared_ptr<NEcs::World> world,
                         NCommon::TaskSystem& tasks,
                         const std::shared_ptr<UpdateCompletion>& completion) {
    auto prepared = NEcs::NInternal::PrepareSystems(
            std::move(world),
            tasks,
            [completion](NEcs::NInternal::SystemRunOutcome result) {
                EUpdateStatus status = EUpdateStatus::COMPLETED;
                if (result.Status == NEcs::NInternal::ESystemRunStatus::FAILED) {
                    status = EUpdateStatus::FAILED;
                } else if (result.Status == NEcs::NInternal::ESystemRunStatus::CANCELLED) {
                    status = EUpdateStatus::CANCELLED;
                }
                completion->Finish({.Status = status, .Error = result.Error});
            });
    const auto run = prepared.GetHandle();
    completion->BindCancellation([run] { run.RequestCancel(); });
    prepared.Start();
}
} // namespace NEngine::NRuntime
