#include <cmath>
#include <numbers>

#include <GraphicsEngine/math/camera.h>
#include <GraphicsEngine/math/transform.h>
#include <gtest/gtest.h>

namespace {

constexpr float EPSILON = 0.0001F;

void ExpectNear(float actual, float expected) {
    EXPECT_NEAR(actual, expected, EPSILON);
}

void ExpectVec3Near(NMath::Vec3 actual, NMath::Vec3 expected) {
    ExpectNear(actual.X, expected.X);
    ExpectNear(actual.Y, expected.Y);
    ExpectNear(actual.Z, expected.Z);
}

} // namespace

TEST(Math, ComposesTransformInScaleRotationTranslationOrder) {
    const NMath::Transform transform{
            .Translation = {.X = 10.0F, .Y = 20.0F, .Z = 30.0F},
            .Rotation = NMath::AngleAxis(std::numbers::pi_v<float> * 0.5F, {.X = 0.0F, .Y = 0.0F, .Z = 1.0F}),
            .Scale = {.X = 2.0F, .Y = 3.0F, .Z = 4.0F},
    };

    const NMath::Vec4 result = NMath::ComposeTransform(transform) * NMath::Vec4{
                                                                            .X = 1.0F,
                                                                            .Y = 0.0F,
                                                                            .Z = 0.0F,
                                                                            .W = 1.0F,
                                                                    };

    ExpectNear(result.X, 10.0F);
    ExpectNear(result.Y, 22.0F);
    ExpectNear(result.Z, 30.0F);
    ExpectNear(result.W, 1.0F);
}

TEST(Math, UsesRightHandedWorldWithForwardNegativeZ) {
    const NMath::Mat4 view = NMath::LookAt({.X = 0.0F, .Y = 0.0F, .Z = 0.0F},
                                           {.X = 0.0F, .Y = 0.0F, .Z = -1.0F},
                                           {.X = 0.0F, .Y = 1.0F, .Z = 0.0F});

    const NMath::Vec4 result = view * NMath::Vec4{
                                              .X = 0.0F,
                                              .Y = 0.0F,
                                              .Z = -5.0F,
                                              .W = 1.0F,
                                      };

    ExpectNear(result.X, 0.0F);
    ExpectNear(result.Y, 0.0F);
    ExpectNear(result.Z, -5.0F);
    ExpectNear(result.W, 1.0F);
}

TEST(Math, UsesVulkanClipSpaceDepthRange) {
    const NMath::Mat4 projection = NMath::Perspective({
            .VerticalFovRadians = std::numbers::pi_v<float> * 0.5F,
            .AspectRatio = 1.0F,
            .NearPlane = 0.1F,
            .FarPlane = 100.0F,
    });

    const NMath::Vec4 nearPoint = projection * NMath::Vec4{
                                                       .X = 0.0F,
                                                       .Y = 0.0F,
                                                       .Z = -0.1F,
                                                       .W = 1.0F,
                                               };
    const NMath::Vec4 farPoint = projection * NMath::Vec4{
                                                      .X = 0.0F,
                                                      .Y = 0.0F,
                                                      .Z = -100.0F,
                                                      .W = 1.0F,
                                              };

    ExpectNear(nearPoint.Z / nearPoint.W, 0.0F);
    ExpectNear(farPoint.Z / farPoint.W, 1.0F);
}

TEST(Math, VectorOperationsUseExpectedAxes) {
    ExpectNear(NMath::Dot({.X = 1.0F, .Y = 2.0F, .Z = 3.0F}, {.X = 4.0F, .Y = 5.0F, .Z = 6.0F}), 32.0F);
    ExpectVec3Near(NMath::Cross({.X = 1.0F, .Y = 0.0F, .Z = 0.0F}, {.X = 0.0F, .Y = 1.0F, .Z = 0.0F}),
                   {.X = 0.0F, .Y = 0.0F, .Z = 1.0F});
    ExpectVec3Near(NMath::Normalize(NMath::Vec3{.X = 0.0F, .Y = 3.0F, .Z = 4.0F}), {.X = 0.0F, .Y = 0.6F, .Z = 0.8F});
}
