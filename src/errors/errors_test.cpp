#include "errors.hpp"

#include <gtest/gtest.h>

#include <any>
#include <memory>
#include <string>
#include <system_error>

namespace amj::errors {
namespace {

TEST(Errors, DefaultHasNoCode) {
    const Error error;
    EXPECT_TRUE(!error.errorCode);
    EXPECT_TRUE(error.stacktrace.empty());
}

TEST(Errors, MatchesCauseWithoutOuterCode) {
    const std::error_code causeErrorCode = std::make_error_code(std::errc::invalid_argument);
    const Error causeError = {
        .errorCode = causeErrorCode,
        .errorMessage = "invalid input",
        .detail = std::string("input detail"),
    };
    const Error error = {
        .errorMessage = "operation failed",
        .cause = std::make_shared<const Error>(causeError),
    };
    EXPECT_FALSE(static_cast<bool>(error.errorCode));
    EXPECT_TRUE(error.is(causeErrorCode));
    EXPECT_FALSE(error.is(std::make_error_code(std::errc::timed_out)));
    const Error* asError = error.as(causeErrorCode);
    ASSERT_EQ(asError, error.cause.get());
    ASSERT_NE(asError, nullptr);
    const std::string* detail = std::any_cast<std::string>(&asError->detail);
    ASSERT_NE(detail, nullptr);
    EXPECT_EQ(*detail, "input detail");
    EXPECT_EQ(error.as(std::make_error_code(std::errc::timed_out)), nullptr);
    EXPECT_EQ(error.toString(), "operation failed: invalid input");
}

TEST(Errors, MatchesFirstCodeInChain) {
    const std::error_code errorCode = std::make_error_code(std::errc::invalid_argument);
    const Error error = {
        .errorCode = errorCode,
        .cause = std::make_shared<const Error>(Error{
            .errorCode = errorCode,
        }),
    };
    EXPECT_TRUE(error.is(errorCode));
    EXPECT_EQ(error.as(errorCode), &error);
}

TEST(Errors, FormatsCodeWithoutMessage) {
    const std::error_code errorCode = std::make_error_code(std::errc::invalid_argument);
    const Error error = {
        .errorCode = errorCode,
    };
    EXPECT_EQ(error.toString(), errorCode.message());
}

TEST(Errors, FormatsCauseStacktrace) {
    const Error error = {
        .errorMessage = "operation failed",
        .cause = std::make_shared<const Error>(Error{
            .errorMessage = "invalid input",
            .stacktrace = {{"input.cpp", 42, "parseInput"}},
        }),
    };
    EXPECT_EQ(error.toString(), "operation failed: invalid input\n  at input.cpp:42 parseInput");
}

TEST(Errors, CapturesStacktrace) {
    const Error error = withStackTrace(Error{
        .errorMessage = "unexpected failure",
    });
    EXPECT_FALSE(error.stacktrace.empty());
    EXPECT_LE(error.stacktrace.size(), 30U);
    EXPECT_EQ(error.errorMessage, "unexpected failure");
}

TEST(Errors, PreservesExistingStacktrace) {
    const Error original = {
        .errorMessage = "unexpected failure",
        .stacktrace = {{"input.cpp", 42, "parseInput"}},
    };
    const Error error = withStackTrace(original);
    EXPECT_EQ(error.stacktrace, original.stacktrace);

    const Error uncaptured = {
        .errorMessage = "operation failed",
        .cause = std::make_shared<const Error>(original),
    };
    const Error wrapper = withStackTrace(uncaptured);
    EXPECT_TRUE(wrapper.stacktrace.empty());
    ASSERT_NE(wrapper.cause, nullptr);
    EXPECT_EQ(wrapper.cause->stacktrace, original.stacktrace);
}

} // namespace
} // namespace amj::errors
