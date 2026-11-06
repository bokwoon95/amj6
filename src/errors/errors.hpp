#pragma once

#include <any>
#include <cstdint>
#include <memory>
#include <string>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <vector>

namespace amj::errors {

enum class ErrorCode : std::uint8_t {
    GenericError = 1,
};

[[nodiscard]] std::error_code make_error_code(ErrorCode errorCode);

// A zero errorCode represents success; every failure must have a nonzero code.
// All modules share this type. Module-specific enums use their own error
// categories; detail may hold any copyable module-specific error struct.
// Treat published errors and their cause chains as immutable and acyclic.
struct Error {
    std::error_code errorCode = {};
    std::string errorMessage = "";
    std::any detail = {};
    std::shared_ptr<const Error> cause = nullptr;

    // (file, line, description); empty unless explicitly captured.
    std::vector<std::tuple<std::string, std::uint_least32_t, std::string>> stacktrace = {};

    // Searches this error and its cause chain for an exact code match.
    [[nodiscard]] bool is(std::error_code targetErrorCode) const;

    // First matching error, or nullptr. Borrowed; must remain alive and unchanged.
    [[nodiscard]] const Error* as(std::error_code targetErrorCode) const;

    // Includes contextual messages, underlying failures, and any captured trace.
    [[nodiscard]] std::string toString() const;
};

[[nodiscard]] Error message(std::string errorMessage);

// Captures at most 30 runtime frames; does nothing if the chain already has a trace.
// Call at the origin of an unexpected failure, not at every propagation site.
// Source locations require debug symbols. Capture/formatting may throw.
[[nodiscard]] Error withStackTrace(Error error);

} // namespace amj::errors

namespace std {

template <>
struct is_error_code_enum<amj::errors::ErrorCode> : true_type {};

} // namespace std
