#define GRAPHICS_ENGINE_LOG_COMPILED_MINIMUM_SEVERITY 5
#include <GraphicsEngine/lib/logging/logger.h>
#include <gtest/gtest.h>

namespace {
TEST(LoggerCompileTime, DisabledSeverityDoesNotEvaluateMessage) {
    NLogging::Logger logger;
    int evaluated = 0;
    GRAPHICS_ENGINE_LOG(logger, NLogging::ESeverity::INFO, "compile", (++evaluated, "disabled"));
    EXPECT_EQ(evaluated, 0);
}
} // namespace
