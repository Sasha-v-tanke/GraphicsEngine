MODULE(VulkanBackend)

API(
    instance.h
)

SOURCES(
    instance.cpp
)

PUBLIC_DEPENDS(
    Common
    Vulkan
)

RECURSE(
    test
)
