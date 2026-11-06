#pragma once

#include "errors/errors.hpp"

#include <chrono>
#include <optional>
#include <stop_token>

namespace amj::context {
namespace chrono = std::chrono;
namespace errors = amj::errors;

// Copies share cancellation state through the token; deadlines are copied.
// A default context has no cancellation source and no deadline.
// This is cooperative: operations must explicitly observe the context.
struct Context {
    std::stop_token cancellation = std::stop_token();
    std::optional<chrono::steady_clock::time_point> deadline = std::nullopt;

    // nullopt means still active. Cancellation takes precedence over timeout.
    // Uses std::errc::operation_canceled and std::errc::timed_out codes.
    [[nodiscard]] std::optional<errors::Error> optionalError() const;
};

} // namespace amj::context
