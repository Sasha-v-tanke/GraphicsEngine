#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>
#include <resources/mesh_loader.h>
#include <resources/mesh_loader_internal.h>
#include <tests/common/test_error.h>

namespace {

using NResources::EResourceState;
using NResources::MeshData;
using NResources::MeshVertex;
using NResources::ResourceIdentity;
using NResources::ResourceManager;

constexpr std::string_view TRIANGLE = "v 0 0 0\nv 1 0 0\nv 0 1 0\n";

class MeshLoader: public testing::Test {
protected:
    void SetUp() override {
        // Exclusive directory creation keeps concurrent test processes isolated.
        for (std::size_t index = 0;; ++index) {
            Directory = std::filesystem::temp_directory_path() / ("graphics-engine-obj-" + std::to_string(index));
            if (std::filesystem::create_directory(Directory)) {
                break;
            }
        }
        Path = Directory / "mesh.obj";
    }

    void TearDown() override {
        std::filesystem::remove_all(Directory);
    }

    void Write(std::string_view text) const {
        std::ofstream file{Path, std::ios::binary};
        file << text;
        ASSERT_TRUE(file.good());
    }

    std::shared_ptr<const MeshData> Load(std::string_view text) {
        Write(text);
        ResourceManager resources;
        const auto handle = NResources::LoadMesh(resources, ResourceIdentity{"mesh", "test"}, Path);
        EXPECT_EQ(resources.GetState(handle), EResourceState::READY);
        if (const auto error = resources.GetFailure(handle)) {
            ADD_FAILURE() << error->Message;
        }
        return resources.GetCpuResource(handle);
    }

    std::filesystem::path Directory;
    std::filesystem::path Path;
};

TEST_F(MeshLoader, PreservesTriangleOrderCoordinatesAndWinding) {
    const auto mesh = Load("v -1 0 2\nv 3 0 2\nv -1 4 2\nf +3 +2 +1\nf 3 2 1\n");
    ASSERT_NE(mesh, nullptr);
    ASSERT_EQ(mesh->GetVertices().size(), 3);
    EXPECT_EQ(mesh->GetIndices(), (std::vector<std::uint32_t>{0, 1, 2, 0, 1, 2}));
    EXPECT_FLOAT_EQ(mesh->GetVertices()[0].Position[1], 4.0F);
    EXPECT_FLOAT_EQ(mesh->GetVertices()[1].Position[0], 3.0F);
    EXPECT_FLOAT_EQ(mesh->GetVertices()[2].Position[0], -1.0F);
    for (const MeshVertex& vertex: mesh->GetVertices()) {
        EXPECT_FLOAT_EQ(vertex.Position[2], 2.0F);
        EXPECT_FLOAT_EQ(vertex.Normal[0], 0.0F);
        EXPECT_FLOAT_EQ(vertex.Normal[1], 0.0F);
        EXPECT_FLOAT_EQ(vertex.Normal[2], 0.0F);
        EXPECT_FLOAT_EQ(vertex.Texcoord[0], 0.0F);
        EXPECT_FLOAT_EQ(vertex.Texcoord[1], 0.0F);
    }
    EXPECT_EQ(mesh->GetAttributeMasks(), (std::vector<std::uint8_t>{0, 0, 0}));
}

TEST_F(MeshLoader, TriangulatesQuadDeterministicallyWithoutChangingWinding) {
    constexpr std::string_view OBJ = "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3 4\n";
    const auto first = Load(OBJ);
    const auto second = Load(OBJ);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_EQ(first->GetVertices().size(), 4);
    EXPECT_EQ(first->GetIndices(), (std::vector<std::uint32_t>{0, 1, 2, 1, 3, 2}));
    EXPECT_EQ(first->GetIndices(), second->GetIndices());
    EXPECT_EQ(first->GetAttributeMasks(), second->GetAttributeMasks());
    for (std::size_t index = 0; index < first->GetVertices().size(); ++index) {
        EXPECT_FLOAT_EQ(first->GetVertices()[index].Position[0], second->GetVertices()[index].Position[0]);
        EXPECT_FLOAT_EQ(first->GetVertices()[index].Position[1], second->GetVertices()[index].Position[1]);
        EXPECT_FLOAT_EQ(first->GetVertices()[index].Position[2], second->GetVertices()[index].Position[2]);
    }
    for (std::size_t index = 0; index < first->GetIndices().size(); index += 3) {
        const std::array<float, 3> a = first->GetVertices()[first->GetIndices()[index]].Position;
        const std::array<float, 3> b = first->GetVertices()[first->GetIndices()[index + 1]].Position;
        const std::array<float, 3> c = first->GetVertices()[first->GetIndices()[index + 2]].Position;
        EXPECT_GT((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]), 0.0F);
    }
}

TEST_F(MeshLoader, KeepsNormalAndUvSeamsAndMixedPresence) {
    const auto mesh = Load(std::string{TRIANGLE} + "vn 0 0 2\nvn 0 0 -2\nvt 0.25 0.75\nvt 0.5 0.125\n"
                                                   "f 1/1/1 2/1/1 3/1/1\nf 1/2/1 2/1/2 3//1\nf 1/1 2 3/1/1\n");
    ASSERT_NE(mesh, nullptr);
    ASSERT_EQ(mesh->GetVertices().size(), 8);
    EXPECT_EQ(mesh->GetIndices(), (std::vector<std::uint32_t>{0, 1, 2, 3, 4, 5, 6, 7, 2}));
    EXPECT_EQ(mesh->GetAttributeMasks(), (std::vector<std::uint8_t>{3, 3, 3, 3, 3, 1, 2, 0}));
    EXPECT_FLOAT_EQ(mesh->GetVertices()[0].Normal[2], 2.0F);
    EXPECT_FLOAT_EQ(mesh->GetVertices()[4].Normal[2], -2.0F);
    EXPECT_FLOAT_EQ(mesh->GetVertices()[0].Texcoord[1], 0.75F);
    EXPECT_FLOAT_EQ(mesh->GetVertices()[3].Texcoord[1], 0.125F);
    EXPECT_FLOAT_EQ(mesh->GetVertices()[5].Texcoord[0], 0.0F);
    EXPECT_FLOAT_EQ(mesh->GetVertices()[5].Texcoord[1], 0.0F);
    EXPECT_FLOAT_EQ(mesh->GetVertices()[6].Normal[2], 0.0F);
    EXPECT_FLOAT_EQ(mesh->GetVertices()[7].Normal[2], 0.0F);
    EXPECT_FLOAT_EQ(mesh->GetVertices()[7].Texcoord[1], 0.0F);
}

TEST_F(MeshLoader, ResolvesRelativeIndicesAndIgnoresNonGeometryWithoutSidecarIo) {
    const auto mesh = Load(std::string{TRIANGLE} +
                           "mtllib missing.mtl\no object\ng first\nusemtl missing\ns 1\nf -3 -2 -1 # triangle\n"
                           "g second\nl 1 2\np 3\nf 1 2 3\n");
    ASSERT_NE(mesh, nullptr);
    EXPECT_EQ(mesh->GetIndices(), (std::vector<std::uint32_t>{0, 1, 2, 0, 1, 2}));
    EXPECT_EQ(mesh->GetVertices().size(), 3);
}

TEST_F(MeshLoader, AcceptsBomCrLfAndDocumentedOptionalComponents) {
    const auto mesh = Load("\xEF\xBB\xBFv +0 0 0 1\r\nv 1 0 0 1 0 0\r\nv 0 1 0 1 0 1 0\r\n"
                           "vt 0.25\r\nvt 0.5 0.75 1\r\nf 1/1 2/2 3/1\r\n");
    ASSERT_NE(mesh, nullptr);
    EXPECT_FLOAT_EQ(mesh->GetVertices()[0].Texcoord[1], 0.0F);
    EXPECT_FLOAT_EQ(mesh->GetVertices()[1].Texcoord[1], 0.75F);
}

class InvalidMesh: public MeshLoader, public testing::WithParamInterface<std::string> {};

TEST_P(InvalidMesh, FailsWithoutPublishingPartialGeometry) {
    Write(GetParam());
    ResourceManager resources;
    const ResourceIdentity identity{"mesh", "invalid-asset"};
    const auto handle = NResources::LoadMesh(resources, identity, Path);
    EXPECT_EQ(resources.GetState(handle), EResourceState::FAILED);
    EXPECT_EQ(resources.GetCpuResource(handle), nullptr);
    EXPECT_FALSE(resources.TryAcquire(handle).has_value());
    const auto error = resources.GetFailure(handle);
    ASSERT_TRUE(error.has_value());
    EXPECT_NE(error->Message.find("invalid-asset"), std::string::npos);
    EXPECT_NE(error->Message.find(Path.string()), std::string::npos);
}

INSTANTIATE_TEST_SUITE_P(CorruptObj,
                         InvalidMesh,
                         testing::Values("",
                                         "not an OBJ",
                                         "v 0 0 0\n",
                                         std::string{TRIANGLE} + "f 1 2\n",
                                         std::string{TRIANGLE} + "f 1 2 0\n",
                                         std::string{TRIANGLE} + "f 1 2 4\n",
                                         std::string{TRIANGLE} + "f 1 2 -4\n",
                                         std::string{TRIANGLE} + "f 1 2 2147483648\n",
                                         std::string{TRIANGLE} + "f 1 2 3x\n",
                                         std::string{TRIANGLE} + "f 1/ 2 3\n",
                                         std::string{TRIANGLE} + "f 1// 2 3\n",
                                         std::string{TRIANGLE} + "f 1/1 2 3\n",
                                         std::string{TRIANGLE} + "f 1//1 2 3\n",
                                         std::string{TRIANGLE} + "vt 0 0\nf 1/0 2 3\n",
                                         std::string{TRIANGLE} + "vn 0 0 1\nf 1//-2 2 3\n",
                                         std::string{TRIANGLE} + "f 1 2 3\nf 1 2 3 99\n",
                                         std::string{TRIANGLE} + "f +-1 2 3\n",
                                         "v +-1 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n",
                                         "v nan 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n",
                                         "v 1e999 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n",
                                         std::string{TRIANGLE} + "vn 0 0 inf\nf 1//1 2//1 3//1\n",
                                         std::string{TRIANGLE} + "vt invalid 0\nf 1/1 2/1 3/1\n",
                                         std::string{TRIANGLE} + "v 1 2\nf 1 2 3\n",
                                         std::string{TRIANGLE} + "vn 1 2\nf 1 2 3\n",
                                         std::string{TRIANGLE} + "vt 0 0 0 0\nf 1 2 3\n",
                                         std::string{TRIANGLE} + "f 1/1/1/1 2 3\n",
                                         std::string{TRIANGLE} + "f 1 2 3\nf 1 2\n"));

TEST_F(MeshLoader, MissingFileFailsAndFailedIdentityCanRetry) {
    ResourceManager resources;
    const ResourceIdentity identity{"mesh", "retry"};
    const auto first = NResources::LoadMesh(resources, identity, Path);
    EXPECT_EQ(resources.GetState(first), EResourceState::FAILED);
    ASSERT_TRUE(resources.GetFailure(first).has_value());
    EXPECT_EQ(resources.GetFailure(first)->Code, NCommon::make_error_code(NCommon::EError::IO_ERROR));
    Write(std::string{TRIANGLE} + "f 1 2 3\n");
    const auto second = NResources::LoadMesh(resources, identity, Path);
    EXPECT_EQ(first, second);
    EXPECT_EQ(resources.GetState(second), EResourceState::READY);
    EXPECT_FALSE(resources.GetFailure(second).has_value());
}

TEST_F(MeshLoader, DuplicateIdentityReusesPayloadAndLeaseSurvivesUnload) {
    Write(std::string{TRIANGLE} + "f 1 2 3\n");
    ResourceManager resources;
    const ResourceIdentity identity{"mesh", "cached"};
    const auto first = NResources::LoadMesh(resources, identity, Path);
    const auto lease = resources.TryAcquire(first);
    ASSERT_TRUE(lease.has_value());
    std::filesystem::remove(Path);
    const auto second = NResources::LoadMesh(resources, identity, Path);
    EXPECT_EQ(first, second);
    EXPECT_EQ(resources.GetCpuResource(first).get(), lease->Get());
    resources.RequestUnload(first);
    EXPECT_EQ(NResources::LoadMesh(resources, identity, Path), first);
    EXPECT_EQ(resources.GetState(first), EResourceState::UNLOADING);
    resources.CompleteUnload(first);
    EXPECT_EQ((*lease)->GetIndices().size(), 3);
    EXPECT_FALSE(resources.TryAcquire(first).has_value());
}

TEST_F(MeshLoader, ActiveAndStaleOperationsFollowResourceManagerGenerationRules) {
    ResourceManager resources;
    const ResourceIdentity identity{"mesh", "stale"};
    const auto handle = resources.Request<MeshData>(identity);
    const auto stale = resources.BeginLoading(handle);
    EXPECT_EQ(NResources::LoadMesh(resources, identity, Path), handle);
    EXPECT_EQ(resources.GetState(handle), EResourceState::LOADING);
    resources.RequestUnload(handle);
    resources.CompleteUnload(handle);
    Write(std::string{TRIANGLE} + "f 1 2 3\n");
    EXPECT_EQ(NResources::LoadMesh(resources, identity, Path), handle);
    const auto ready = resources.GetCpuResource(handle);
    NTest::ExpectError(NCommon::EError::INVALID_STATE,
                       [&] { resources.PublishReady(stale, std::make_shared<MeshData>()); });
    EXPECT_EQ(resources.GetCpuResource(handle), ready);
    EXPECT_EQ(resources.GetState(handle), EResourceState::READY);
}

TEST(MeshLimits, ChecksUint32BoundaryWithoutAllocatingHugeMeshes) {
    using NResources::NMeshLoaderInternal::CanAppendMeshElements;
    constexpr std::size_t LIMIT = std::numeric_limits<std::uint32_t>::max();
    EXPECT_TRUE(CanAppendMeshElements(LIMIT - 3, 3));
    EXPECT_FALSE(CanAppendMeshElements(LIMIT - 2, 3));
    EXPECT_TRUE(CanAppendMeshElements(LIMIT - 1, 1));
    EXPECT_FALSE(CanAppendMeshElements(LIMIT, 1));
    EXPECT_FALSE(CanAppendMeshElements(0, LIMIT + 1));
    EXPECT_FALSE(CanAppendMeshElements(std::numeric_limits<std::size_t>::max(), 1));
}

static_assert(std::is_same_v<decltype(std::declval<const MeshData&>().GetVertices()), const std::vector<MeshVertex>&>);

} // namespace
