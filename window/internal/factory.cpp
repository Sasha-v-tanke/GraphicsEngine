#include "factory.h"

#include <memory>

#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>
#include <window/internal/engine.h>

#ifdef GRAPHICS_ENGINE_WINDOW_HAS_GLFW
    #include <window/internal/glfw/window_engine.h>
#endif

namespace NWindow::NInternal {

namespace {

thread_local WindowEngineFactory g_testFactory = nullptr;

[[nodiscard]] std::unique_ptr<IWindowEngine> ValidateWindowEngine(std::unique_ptr<IWindowEngine> engine) {
    if (engine == nullptr) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Window engine factory returned null");
    }

    return engine;
}

[[noreturn]] void ThrowUnavailable(EWindowType type) {
    switch (type) {
    case EWindowType::GLFW:
        GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED, "GLFW window support is not available in this build");
    case EWindowType::QT:
        GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED, "Qt window support is not available in this build");
    case EWindowType::SDL:
        GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED, "SDL window support is not available in this build");
    }

    GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Unknown window type");
}

} // namespace

std::unique_ptr<IWindowEngine> CreateWindowEngine(const WindowConfig& config) {
    if (g_testFactory != nullptr) {
        return ValidateWindowEngine(g_testFactory(config));
    }

#ifdef GRAPHICS_ENGINE_WINDOW_HAS_GLFW
    if (config.Type == EWindowType::GLFW) {
        return ValidateWindowEngine(NGlfw::CreateWindowEngine(config));
    }
#endif

    ThrowUnavailable(config.Type);
}

void SetWindowEngineFactoryForTests(WindowEngineFactory factory) {
    g_testFactory = factory;
}

} // namespace NWindow::NInternal
