#include <source_location>
#include <string_view>
#include <system_error>

#include <gtest/gtest.h>
#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace {

TEST(Error, CreatesErrorCode) {
    const std::error_code code = NCommon::MakeErrorCode(NCommon::EError::INVALID_ARGUMENT);

    EXPECT_EQ(code.value(), static_cast<int>(NCommon::EError::INVALID_ARGUMENT));

    EXPECT_EQ(std::string_view{code.category().name()}, "graphics_engine.common");

    EXPECT_EQ(code.message(), "Invalid argument");

    EXPECT_TRUE(code);
}

TEST(Error, ComparesErrorCodes) {
    const std::error_code first = NCommon::MakeErrorCode(NCommon::EError::INVALID_STATE);

    const std::error_code second = NCommon::MakeErrorCode(NCommon::EError::INVALID_STATE);

    const std::error_code different = NCommon::MakeErrorCode(NCommon::EError::INVALID_ARGUMENT);

    EXPECT_EQ(first, second);
    EXPECT_NE(first, different);
}

TEST(Exception, ThrowsFormattedMessage) {
    try {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Frame index {} is out of range [0, {})", 7, 2);
    } catch (const NCommon::Exception& exception) {
        EXPECT_EQ(exception.code(), NCommon::MakeErrorCode(NCommon::EError::INVALID_ARGUMENT));

        EXPECT_EQ(exception.GetMessage(), "Frame index 7 is out of range [0, 2)");

        EXPECT_NE(std::string_view{exception.what()}.find(exception.GetMessage()), std::string_view::npos);

        return;
    }

    FAIL() << "GRAPHICS_ENGINE_THROW did not throw";
}

TEST(Exception, ThrowsWithoutFormatArguments) {
    try {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Invalid state");
    } catch (const NCommon::Exception& exception) {
        EXPECT_EQ(exception.GetMessage(), "Invalid state");

        return;
    }

    FAIL() << "GRAPHICS_ENGINE_THROW did not throw";
}

TEST(Exception, CapturesSourceLocation) {
    try {
        GRAPHICS_ENGINE_THROW(NCommon::EError::UNKNOWN, "Source location test");
    } catch (const NCommon::Exception& exception) {
        const std::source_location& location = exception.GetLocation();

        EXPECT_NE(location.line(), 0);
        EXPECT_TRUE(std::string_view{location.file_name()}.ends_with("error_test.cpp"));
        EXPECT_FALSE(std::string_view{location.function_name()}.empty());

        return;
    }

    FAIL() << "GRAPHICS_ENGINE_THROW did not throw";
}

TEST(Exception, AcceptsErrorCode) {
    const std::error_code code = NCommon::MakeErrorCode(NCommon::EError::IO_ERROR);

    try {
        GRAPHICS_ENGINE_THROW(code, "Failed to read '{}'", "resource.bin");
    } catch (const NCommon::Exception& exception) {
        EXPECT_EQ(exception.code(), code);

        EXPECT_EQ(exception.GetMessage(), "Failed to read 'resource.bin'");

        return;
    }

    FAIL() << "GRAPHICS_ENGINE_THROW did not throw";
}

} // namespace
