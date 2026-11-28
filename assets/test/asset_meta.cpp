#include "GraphicsEngine/assets/asset_meta.h"

#include <filesystem>
#include <fstream>
#include <thread>

#include <gtest/gtest.h>

namespace {

using NAssets::AssetId;
using NAssets::AssetMeta;

TEST(AssetIdTest, ParsesOnlyCanonical128BitText) {
    const auto id = AssetId::Parse("0123456789abcdef0123456789abcdef");
    ASSERT_TRUE(id);
    EXPECT_EQ(id->ToString(), "0123456789abcdef0123456789abcdef");
    EXPECT_FALSE(AssetId::Parse("01234567-89ab-cdef-0123-456789abcdef"));
    EXPECT_FALSE(AssetId::Parse("0123456789ABCDEF0123456789abcdef"));
}

TEST(AssetMetaTest, CreateThenRenameAndMovePreservesIdentity) {
    const auto root = std::filesystem::temp_directory_path() / "graphics-engine-asset-meta-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "moved");
    const auto source = root / "source.obj";
    std::ofstream(source) << "source";
    const auto id = *AssetId::Parse("0123456789abcdef0123456789abcdef");
    ASSERT_TRUE(NAssets::CreateMeta(
            source,
            AssetMeta{.Id = id, .Importer = "obj", .SchemaVersion = 1, .Settings = {{"scale", "1"}}}));
    std::filesystem::rename(source, root / "moved" / "renamed.obj");
    std::filesystem::rename(NAssets::MetaPathFor(source), NAssets::MetaPathFor(root / "moved" / "renamed.obj"));
    const auto metadata = NAssets::ReadMeta(root / "moved" / "renamed.obj");
    ASSERT_TRUE(metadata);
    EXPECT_EQ(metadata->Id, id);
    EXPECT_EQ(metadata->Settings.at("scale"), "1");
    std::filesystem::remove_all(root);
}

TEST(AssetMetaTest, RejectsMalformedAndDuplicateIdentityFields) {
    const auto root = std::filesystem::temp_directory_path() / "graphics-engine-asset-meta-invalid";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto source = root / "asset.bin";
    std::ofstream(source) << "asset";
    std::ofstream(NAssets::MetaPathFor(source)) << "asset_id=bad\nimporter=x\nschema_version=1\n";
    EXPECT_FALSE(NAssets::ReadMeta(source));
    std::ofstream(NAssets::MetaPathFor(source), std::ios::trunc)
            << "asset_id=0123456789abcdef0123456789abcdef\nasset_id=0123456789abcdef0123456789abcdef\nimporter="
               "x\nschema_version=1\n";
    EXPECT_FALSE(NAssets::ReadMeta(source));
    std::filesystem::remove_all(root);
}

TEST(AssetMetaTest, RejectsUnserializableFieldsWithoutPublishingMetadata) {
    const auto root = std::filesystem::temp_directory_path() / "graphics-engine-asset-meta-fields";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto source = root / "asset.bin";
    std::ofstream(source) << "asset";
    const auto id = *AssetId::Parse("0123456789abcdef0123456789abcdef");
    EXPECT_FALSE(NAssets::CreateMeta(source, AssetMeta{.Id = id, .Importer = "obj", .Settings = {{"scale", ""}}}));
    EXPECT_FALSE(NAssets::CreateMeta(source, AssetMeta{.Id = id, .Importer = "obj\nother", .Settings = {}}));
    EXPECT_FALSE(std::filesystem::exists(NAssets::MetaPathFor(source)));
    std::filesystem::remove_all(root);
}

TEST(AssetMetaTest, ConcurrentCreatePublishesExactlyOneIdentity) {
    const auto root = std::filesystem::temp_directory_path() / "graphics-engine-asset-meta-concurrent";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto source = root / "asset.bin";
    std::ofstream(source) << "asset";
    const auto first = *AssetId::Parse("0123456789abcdef0123456789abcdef");
    const auto second = *AssetId::Parse("fedcba9876543210fedcba9876543210");
    std::expected<void, std::string> one = std::unexpected("not run");
    std::expected<void, std::string> two = std::unexpected("not run");
    std::thread firstThread(
            [&] { one = NAssets::CreateMeta(source, AssetMeta{.Id = first, .Importer = "obj", .SchemaVersion = 1}); });
    std::thread secondThread(
            [&] { two = NAssets::CreateMeta(source, AssetMeta{.Id = second, .Importer = "obj", .SchemaVersion = 1}); });
    firstThread.join();
    secondThread.join();
    EXPECT_NE(static_cast<bool>(one), static_cast<bool>(two));
    const auto metadata = NAssets::ReadMeta(source);
    ASSERT_TRUE(metadata);
    EXPECT_TRUE(metadata->Id == first || metadata->Id == second);
    std::filesystem::remove_all(root);
}

} // namespace
