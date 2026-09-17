#include "factory.h"

#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>
#include <window/internal/engine.h>
#include <window/internal/event_sink.h>

namespace NWindow::NInternal {

namespace {

thread_local WindowEngineFactory g_testFactory = nullptr;

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

std::unique_ptr<IWindowEngine> CreateWindowEngine(const WindowConfig& config, IWindowEventSink& eventSink) {
    if (g_testFactory != nullptr) {
        return g_testFactory(config, eventSink);
    }

    ThrowUnavailable(config.Type);
}

void SetWindowEngineFactoryForTests(WindowEngineFactory factory) {
    g_testFactory = factory;
}

} // namespace NWindow::NInternal
