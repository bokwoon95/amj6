#include "context.hpp"

#include "errors/errors.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <system_error>

namespace amj::context {
namespace chrono = std::chrono;
namespace errors = amj::errors;

std::optional<errors::Error> Context::optionalError() const {
    if (cancellation.stop_requested()) {
        return errors::Error{
            .errorCode = std::make_error_code(std::errc::operation_canceled),
            .errorMessage = std::make_shared<const std::string>("context canceled"),
        };
    }
    if (deadline.has_value() && chrono::steady_clock::now() >= deadline.value()) {
        return errors::Error{
            .errorCode = std::make_error_code(std::errc::timed_out),
            .errorMessage = std::make_shared<const std::string>("context deadline exceeded"),
        };
    }
    return std::nullopt;
}

} // namespace amj::context
