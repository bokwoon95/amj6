#include "errors.hpp"

#include <gtest/gtest.h>

#include <any>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

namespace amj::errors {
namespace {

TEST(Errors, DefaultHasNoCode) {
    const Error error;
    EXPECT_FALSE(error.hasValue());
    EXPECT_EQ(error.stacktrace, nullptr);
}

TEST(Errors, MatchesCauseWithoutOuterCode) {
    const std::error_code causeErrorCode = std::make_error_code(std::errc::invalid_argument);
    const Error causeError = {
        .errorCode = causeErrorCode,
        .errorMessage = std::make_shared<const std::string>("invalid input"),
        .detail = std::make_shared<const std::any>(std::string("input detail")),
    };
    const Error error = {
        .errorMessage = std::make_shared<const std::string>("operation failed"),
        .cause = std::make_shared<const Error>(causeError),
    };
    EXPECT_FALSE(error.hasValue());
    EXPECT_TRUE(error.is(causeErrorCode));
    EXPECT_FALSE(error.is(std::make_error_code(std::errc::timed_out)));
    const Error* asError = error.as(causeErrorCode);
    ASSERT_EQ(asError, error.cause.get());
    ASSERT_NE(asError, nullptr);
    const std::string* detail = std::any_cast<std::string>(asError->detail.get());
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
    const Error causeError = {
        .errorMessage = std::make_shared<const std::string>("invalid input"),
        .stacktrace = std::make_shared<const std::vector<std::tuple<std::string, std::uint_least32_t, std::string>>>(std::initializer_list<std::tuple<std::string, std::uint_least32_t, std::string>>{{"input.cpp", 42, "parseInput"}}),
    };
    const Error error = {
        .errorMessage = std::make_shared<const std::string>("operation failed"),
        .cause = std::make_shared<const Error>(causeError),
    };
    EXPECT_EQ(error.toString(), "operation failed: invalid input\n  at input.cpp:42 parseInput");
} // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks) -- MSVC shared_ptr false positive; all payloads are owned by this acyclic chain.

TEST(Errors, CapturesStacktrace) {
    const Error error = withStackTrace(Error{
        .errorMessage = std::make_shared<const std::string>("unexpected failure"),
    });
    ASSERT_NE(error.stacktrace, nullptr);
    EXPECT_FALSE(error.stacktrace->empty());
    EXPECT_LE(error.stacktrace->size(), 30U);
    ASSERT_NE(error.errorMessage, nullptr);
    EXPECT_EQ(*error.errorMessage, "unexpected failure");
}

TEST(Errors, PreservesExistingStacktrace) {
    const Error original = {
        .errorMessage = std::make_shared<const std::string>("unexpected failure"),
        .stacktrace = std::make_shared<const std::vector<std::tuple<std::string, std::uint_least32_t, std::string>>>(std::initializer_list<std::tuple<std::string, std::uint_least32_t, std::string>>{{"input.cpp", 42, "parseInput"}}),
    };
    const Error error = withStackTrace(original);
    EXPECT_EQ(error.stacktrace, original.stacktrace);

    const Error uncaptured = {
        .errorMessage = std::make_shared<const std::string>("operation failed"),
        .cause = std::make_shared<const Error>(original),
    };
    const Error wrapper = withStackTrace(uncaptured);
    EXPECT_EQ(wrapper.stacktrace, nullptr);
    ASSERT_NE(wrapper.cause, nullptr);
    EXPECT_EQ(wrapper.cause->stacktrace, original.stacktrace);
}

} // namespace
} // namespace amj::errors
