TEST(VulkanLibs)

SOURCES(
    vulkan_libs.cpp
)

PRIVATE_DEPENDS(
    Vulkan
)

TEST_LABELS(
    small
    cpp
    libs
)