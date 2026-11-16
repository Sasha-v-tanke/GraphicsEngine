#include "forest.h"

#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <ecs/internal/scoped_batch.h>
#include <engine/controller/frame_scheduler.h>
#include <graphics/material.h>
#include <gtest/gtest.h>
#include <renderer/extraction.h>
#include <resources/mesh_data.h>
#include <scene/scene_manager.h>

namespace NEcsAcceptance {
namespace {
class Frames: public testing::TestWithParam<std::pair<unsigned, std::size_t>> {
protected:
    struct Source {
        std::shared_ptr<NScene::Scene> Scene;
        std::unique_ptr<Forest> Model;
        std::vector<float> Offsets;
    };
    struct Expected {
        NRenderer::RenderObjectId Id;
        Matrix Transform;
        std::uint64_t MaterialVersion;
    };
    struct Pinned {
        NEngine::NController::FrameHandle Frame;
        const NRenderer::RenderWorld* Snapshot = nullptr;
        std::vector<Expected> Values;
    };
    NResources::ResourceManager Resources;
    NScene::SceneManager Scenes;
    std::array<NScene::SceneHandle, 2> Handles;
    std::array<Source, 2> Sources;
    std::unique_ptr<NCommon::TaskSystem> Tasks;
    std::unique_ptr<NEngine::NController::FrameScheduler> Scheduler;
    NRenderer::NInternal::IncrementalExtraction Cache;
    NResources::ResourceHandle<NResources::MeshData> Mesh;
    NResources::ResourceHandle<NGraphics::Material> Material;
    std::vector<Pinned> Active;
    std::uint64_t PublishedVersion = 0;
    bool Ready = true;

    void Publish() {
        Resources.PublishReady(
                Resources.BeginLoading(Material),
                std::make_shared<NGraphics::Material>(NResources::ResourceIdentity{"pipeline", "acceptance"}));
        ++PublishedVersion;
        Ready = true;
    }
    void CreateSource(std::size_t index) {
        Handles[index] = Scenes.CreateScene(std::to_string(index));
        auto& source = Sources[index];
        source.Scene = Scenes.Acquire(Handles[index]);
        source.Model =
                std::make_unique<Forest>(source.Scene->GetWorld(), 96, index == 0 ? EShape::BALANCED : EShape::SKEWED);
        source.Offsets.assign(source.Model->Entities.size(), 1);
        auto& world = source.Scene->GetWorld();
        for (const auto entity: source.Model->Entities) {
            world.AddComponent<NRenderer::TransformComponent>(
                    entity.Identity,
                    NRenderer::TransformComponent{{.Translation = {1, 0, 0}}});
            world.AddComponent<NRenderer::RenderableComponent>(
                    entity.Identity,
                    NRenderer::RenderableComponent{.Mesh = Mesh, .Material = Material});
        }
        const auto camera = world.CreateEntity();
        world.AddComponent<NRenderer::TransformComponent>(camera);
        world.AddComponent<NRenderer::CameraComponent>(camera);
        const auto entity = source.Model->Entities.back();
        NEcs::NInternal::ScopedWorldAccess::Register(
                world,
                {NEcs::NInternal::ScopedWrite<NRenderer::TransformComponent>(
                        NEcs::NInternal::BatchScope::Entity(entity))},
                [entity](NEcs::NInternal::ScopedBatchContext& context) {
                    context.Write<NRenderer::TransformComponent>().Get(entity).Value.Translation.X += 1;
                },
                NEcs::SystemSchedule{.Id = 1});
    }
    void SetUp() override {
        Tasks = std::make_unique<NCommon::TaskSystem>(GetParam().first);
        Scheduler = std::make_unique<NEngine::NController::FrameScheduler>(
                NEngine::EngineConfig{.MaxActiveFrames = GetParam().second});
        Mesh = Resources.Request<NResources::MeshData>({"mesh", "acceptance"});
        Resources.PublishReady(Resources.BeginLoading(Mesh), std::make_shared<NResources::MeshData>());
        Material = Resources.Request<NGraphics::Material>({"material", "acceptance"});
        Publish();
        CreateSource(0);
        CreateSource(1);
    }
    void VerifyPinned() const {
        for (const Pinned& pinned: Active) {
            if (!pinned.Snapshot) {
                continue;
            }
            const auto objects = pinned.Snapshot->GetObjects();
            ASSERT_EQ(objects.size(), pinned.Values.size());
            for (std::size_t index = 0; index < objects.size(); ++index) {
                const auto& object = objects[index];
                const auto& expected = pinned.Values[index];
                EXPECT_EQ(object.Id, expected.Id);
                EXPECT_EQ(object.Material.GetVersion(), expected.MaterialVersion);
                ASSERT_TRUE(object.ResolvedWorldTransform);
                for (std::size_t row = 0; row < 4; ++row) {
                    for (std::size_t column = 0; column < 4; ++column) {
                        EXPECT_FLOAT_EQ((*object.ResolvedWorldTransform)(row, column),
                                        expected.Transform[row * 4 + column]);
                    }
                }
            }
        }
    }
    enum class EAttempt {
        SUCCESS,
        CANCEL,
        FAIL
    };
    const NRenderer::RenderWorld* Extract(std::size_t index, EAttempt attempt = EAttempt::SUCCESS) {
        VerifyPinned();
        if (Active.size() == GetParam().second) {
            Scheduler->CompleteFrame(Active.front().Frame);
            Scheduler->RecycleFrame(Active.front().Frame);
            Active.erase(Active.begin());
        }
        const auto frame = *Scheduler->TryAcquireFrame();
        Scheduler->ArmFrame(frame);
        Scheduler->BeginUpdate(frame);
        Scheduler->EndUpdate(frame);
        Scheduler->SignalDraw(frame);
        Scheduler->BeginFinalize(frame);
        Scenes.Activate(Handles[index]);
        auto active = Scenes.AcquireActive();
        const NRenderer::RenderWorld* snapshot = nullptr;
        NCommon::TaskHandle join;
        const auto root = Tasks->Submit([&](NCommon::TaskContext& task) {
            join = Cache.Schedule(active->GetWorld(),
                                  Resources,
                                  Scheduler->GetFrameStorage(frame),
                                  *Tasks,
                                  task,
                                  [&](const auto& result) {
                                      if (attempt == EAttempt::FAIL) {
                                          throw std::runtime_error{"acceptance publication failure"};
                                      }
                                      snapshot = &result;
                                  });
            if (attempt == EAttempt::CANCEL) {
                Tasks->Cancel(join);
            }
        });
        Tasks->Wait(root);
        if (Tasks->GetStatus(root) != NCommon::ETaskStatus::COMPLETED) {
            throw std::runtime_error{Tasks->GetError(root)->Message};
        }
        Tasks->Wait(join);
        Tasks->WaitIdle();
        const auto expectedStatus = attempt == EAttempt::SUCCESS ? NCommon::ETaskStatus::COMPLETED
                                  : attempt == EAttempt::CANCEL  ? NCommon::ETaskStatus::CANCELLED
                                                                 : NCommon::ETaskStatus::FAILED;
        EXPECT_EQ(Tasks->GetStatus(join), expectedStatus);
        Pinned pinned;
        pinned.Frame = frame;
        pinned.Snapshot = snapshot;
        if (attempt == EAttempt::SUCCESS) {
            EXPECT_NE(snapshot, nullptr);
            const auto& source = Sources[index];
            const auto matrices = source.Model->ResolveReference();
            if (Ready) {
                for (std::size_t entity = 0; entity < matrices.size(); ++entity) {
                    Matrix component = Identity();
                    component[3] = source.Offsets[entity];
                    const auto reference = source.Model->Entities[entity].Identity;
                    pinned.Values.push_back({{reference.Index, reference.Generation},
                                             Multiply(matrices[entity], component),
                                             PublishedVersion});
                }
            }
        } else {
            EXPECT_EQ(snapshot, nullptr);
        }
        Active.push_back(std::move(pinned));
        VerifyPinned();
        return snapshot;
    }
};

TEST_P(Frames, ScopedWritesReparentResourceRetryAndSceneUnloadPreserveActiveSnapshots) {
    for (std::size_t round = 0; round < 12; ++round) {
        const std::size_t index = (round / 3) % Sources.size();
        auto& source = Sources[index];
        auto& world = source.Scene->GetWorld();
        world.RunSystems(*Tasks);
        source.Offsets.back() += 1;
        Matrix parent = Identity();
        parent[3] = static_cast<float>(round);
        parent[1] = 0.25F;
        source.Model->SetLocal(0, parent);
        source.Model->Reparent(source.Model->Entities.size() - 1, round % 7);
        if (round % 4 == 1) {
            Resources.RequestUnload(Material);
            Resources.CompleteUnload(Material);
            Resources.Fail(Resources.BeginLoading(Material),
                           {.Code = NCommon::EError::IO_ERROR, .Message = "seeded delayed material failure"});
            Ready = false;
            const auto* skipped = Extract(index);
            ASSERT_NE(skipped, nullptr);
            EXPECT_FALSE(skipped->GetResourceDiagnostics().empty());
            Publish();
        }
        if (round == 2) {
            (void)Extract(index, EAttempt::CANCEL);
        }
        if (round == 5) {
            (void)Extract(index, EAttempt::FAIL);
        }
        const auto* current = Extract(index);
        ASSERT_NE(current, nullptr);
        EXPECT_EQ(current->GetObjects().size(), source.Model->Entities.size());
        const auto* hot = Extract(index);
        ASSERT_NE(hot, nullptr);
        EXPECT_TRUE(hot->GetDelta().UpdatedObjects.empty());
        EXPECT_EQ(Cache.GetStatistics().RecomputedTransforms, 0U);
        if (round == 8) {
            std::weak_ptr<NScene::Scene> expired = source.Scene;
            source.Model.reset();
            source.Scene.reset();
            Scenes.Unload(Handles[index]);
            EXPECT_TRUE(expired.expired());
            VerifyPinned();
            CreateSource(index);
        }
    }
    VerifyPinned();
    RecordProperty("workers", GetParam().first);
    RecordProperty("active_slots", static_cast<int>(GetParam().second));
}

std::vector<std::pair<unsigned, std::size_t>> MatrixParameters() {
    std::vector<std::pair<unsigned, std::size_t>> values;
    for (const unsigned workers: WORKERS) {
        for (const std::size_t slots: {1U, 2U, 3U}) {
            values.emplace_back(workers, slots);
        }
    }
    return values;
}
INSTANTIATE_TEST_SUITE_P(WorkersAndSlots, Frames, testing::ValuesIn(MatrixParameters()));
} // namespace
} // namespace NEcsAcceptance
