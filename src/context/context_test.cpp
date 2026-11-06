#include "context.hpp"

#include "errors/errors.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <stop_token>
#include <system_error>

namespace amj::context {
namespace chrono = std::chrono;
namespace errors = amj::errors;
namespace {

TEST(Context, DefaultHasNoError) {
    const Context context;
    EXPECT_FALSE(context.optionalError().has_value());
}

TEST(Context, ExpiredDeadlineTimesOut) {
    const Context context = {
        .cancellation = std::stop_token(),
        .deadline = chrono::steady_clock::now(),
    };
    const std::optional<errors::Error> optionalError = context.optionalError();
    if (!optionalError.has_value()) {
        FAIL() << "expected a context error";
        return;
    }
    const errors::Error& error = optionalError.value();
    EXPECT_EQ(error.errorCode, std::make_error_code(std::errc::timed_out));
    EXPECT_EQ(error.errorMessage, "context deadline exceeded");
    EXPECT_TRUE(error.stacktrace.empty());
}

TEST(Context, CancellationTakesPrecedenceOverTimeout) {
    std::stop_source stopSource;
    const Context context = {
        .cancellation = stopSource.get_token(),
        .deadline = chrono::steady_clock::now(),
    };
    stopSource.request_stop();
    const std::optional<errors::Error> optionalError = context.optionalError();
    if (!optionalError.has_value()) {
        FAIL() << "expected a context error";
        return;
    }
    const errors::Error& error = optionalError.value();
    EXPECT_EQ(error.errorCode, std::make_error_code(std::errc::operation_canceled));
    EXPECT_EQ(error.errorMessage, "context canceled");
    EXPECT_TRUE(error.stacktrace.empty());
}

} // namespace
} // namespace amj::context
