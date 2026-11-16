#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include <GraphicsEngine/ecs/entity_ref.h>
#include <GraphicsEngine/ecs/name_component.h>
#include <GraphicsEngine/renderer/render_components.h>
#include <GraphicsEngine/scene/scene.h>
#include <gtest/gtest.h>

namespace {
struct Health {
    int Current;
    int Maximum;
    void Damage(int value) {
        Current -= value;
    }
};
struct Velocity {
    float X;
    float Y;
};

static_assert(std::is_same_v<decltype(std::declval<NScene::Scene&>().CreateEntity()), NEcs::EntityRef>);

TEST(SceneEntity, FluentTemporaryAndLvalueUseOnlyTheOwnedWorld) {
    NScene::Scene scene;
    auto player = scene.CreateEntity("Player")
                          .AddComponent<Health>(100, 100)
                          .AddComponent<Velocity>(Velocity{1.0f, 2.0f})
                          .AddComponent<std::unique_ptr<int>>(std::make_unique<int>(7));
    auto other = scene.CreateEntity("Player");
    auto unnamed = scene.CreateEntity();
    EXPECT_TRUE(player.IsPending());
    EXPECT_EQ(scene.GetWorld().GetAliveEntityCount(), 0U);
    scene.CommitPending();
    EXPECT_EQ(scene.GetWorld().GetAliveEntityCount(), 3U);
    EXPECT_EQ(player.GetComponent<NEcs::NameComponent>().Value, "Player");
    EXPECT_EQ(other.GetComponent<NEcs::NameComponent>().Value, "Player");
    EXPECT_NE(player.GetReference(), other.GetReference());
    EXPECT_FALSE(unnamed.HasComponent<NEcs::NameComponent>());
    EXPECT_FALSE(player.HasComponent<NRenderer::TransformComponent>());
    player.GetComponent<Health>().Damage(30);
    EXPECT_EQ(scene.GetWorld().GetComponent<Health>(player.GetReference().Identity).Current, 70);
    auto copy = other.AddComponent<Velocity>(3.0f, 4.0f);
    EXPECT_EQ(copy.GetReference(), other.GetReference());
    EXPECT_FALSE(other.HasComponent<Velocity>());
    scene.CommitPending();
    EXPECT_EQ(other.GetComponent<Velocity>().X, 3.0f);
    EXPECT_EQ(*player.GetComponent<std::unique_ptr<int>>(), 7);
}

TEST(SceneEntity, FacadeCopiesDoNotRetainSceneOrWorld) {
    NEcs::EntityRef entity;
    std::weak_ptr<NScene::Scene> weak;
    {
        const auto scene = std::make_shared<NScene::Scene>();
        weak = scene;
        entity = scene->CreateEntity("Temporary");
        scene->CommitPending();
        EXPECT_TRUE(entity.IsAlive());
    }
    EXPECT_TRUE(weak.expired());
    EXPECT_FALSE(entity.IsAlive());
    EXPECT_FALSE(entity.IsPending());
}
} // namespace
