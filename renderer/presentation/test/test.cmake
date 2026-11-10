TEST_SUITE(GraphPresentation)
SOURCES(presentation.cpp model_visibility.h scene_switching.cpp)
PRIVATE_DEPENDS(Application GraphPresentation Engine Ecs VulkanBackend)
TEST_LABELS(heavy cpp renderer vulkan)
set_tests_properties(GraphicsEngineGraphPresentationTests PROPERTIES TIMEOUT 80
    ENVIRONMENT "VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT")
