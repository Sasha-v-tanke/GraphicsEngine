#include <graphics/backend/backend.h>
#include <graphics/backend/factory.h>
#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NGraphics::NBackend {

namespace {

GraphicsBackendFactory g_graphicsBackendFactory = nullptr;

std::unique_ptr<IGraphicsBackend> CreateDefaultGraphicsBackend(const GraphicsConfig&) {
    GRAPHICS_ENGINE_THROW(NCommon::EError::NOT_IMPLEMENTED, "Default graphics backend is not registered");
}

} // namespace

std::unique_ptr<IGraphicsBackend> CreateGraphicsBackend(const GraphicsConfig& config) {
    auto factory = g_graphicsBackendFactory;

    if (factory == nullptr) {
        factory = &CreateDefaultGraphicsBackend;
    }

    auto backend = factory(config);

    if (backend == nullptr) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Graphics backend factory returned null backend");
    }

    return backend;
}

void SetGraphicsBackendFactoryForTests(GraphicsBackendFactory factory) noexcept {
    g_graphicsBackendFactory = factory;
}

} // namespace NGraphics::NBackend
