TEST(ExternalSmoke)

SOURCES(
    external_smoke.cpp
)

PRIVATE_DEPENDS(
    GraphicsEngine::GLFW
    GraphicsEngine::GLM
    GraphicsEngine::StbImage
    GraphicsEngine::TinyObj
    GraphicsEngine::Vulkan
)

TEST_LABELS(
    medium
)
