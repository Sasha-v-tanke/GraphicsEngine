TEST_SUITE(Application)

TEST(Application)

SOURCES(
    view_registry.cpp
    main_executor.cpp
    application_test.cpp
    runtime_mvp_test.cpp
    presentation_runtime.cpp
    scene_runtime.cpp
    game_frame_bridge.cpp
    post_update.cpp
    service_manager.cpp
)

PRIVATE_DEPENDS(
    Application
    Engine
)

TEST_LABELS(
    small
    cpp
    application
)

if (GRAPHICS_ENGINE_BUILD_VULKAN AND GRAPHICS_ENGINE_BUILD_GLFW)
    TEST_SUITE(ApplicationPresentation)
    TEST(ApplicationPresentation)
    SOURCES(presentation_heavy.cpp)
    PRIVATE_DEPENDS(Application VulkanBackend GLFW)
    TEST_LABELS(heavy cpp application vulkan)
    set_tests_properties(GraphicsEngineApplicationPresentationTests PROPERTIES TIMEOUT 45
        ENVIRONMENT "VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT")
endif ()
