#include <graphics/backend/backend.h>
#include <graphics/backend/factory.h>
#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NGraphics::NBackend {

std::unique_ptr<IGraphicsBackend> CreateDefaultGraphicsBackend(const GraphicsConfig&) {
    GRAPHICS_ENGINE_THROW(NCommon::EError::NOT_IMPLEMENTED, "Default graphics backend is not registered");
}

std::unique_ptr<IGraphicsBackend> CreateGraphicsBackend(const GraphicsConfig& config) {
    return CreateDefaultGraphicsBackend(config);
}

Graphics CreateGraphicsForBackend(std::unique_ptr<IGraphicsBackend> backend,
                                  const RequiredGraphicsCapabilities& requiredCapabilities) {
    return Graphics{std::move(backend), requiredCapabilities};
}

} // namespace NGraphics::NBackend
