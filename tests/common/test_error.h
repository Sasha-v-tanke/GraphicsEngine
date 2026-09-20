#pragma once

#include <gtest/gtest.h>
#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NTest {

template<typename TCallable>
void ExpectError(NCommon::EError expectedError, TCallable&& callable) {
    try {
        callable();
    } catch (const NCommon::Exception& exception) {
        EXPECT_EQ(exception.code(), NCommon::make_error_code(expectedError));

        return;
    }

    FAIL() << "Expected NCommon::Exception";
}

} // namespace NTest
