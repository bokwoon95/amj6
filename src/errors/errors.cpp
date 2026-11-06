#include "errors.hpp"

#include <cpptrace/basic.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>

namespace amj::errors {
namespace {

class ErrorCategory final : public std::error_category {
  public:
    const char* name() const noexcept override {
        return "amj.errors";
    }

    std::string message(int value) const override {
        switch (value) {
            case static_cast<int>(ErrorCode::GenericError):
                return "Operation failed";
            default:
                return "Unknown error";
        }
    }
};

} // namespace

std::error_code make_error_code(ErrorCode errorCode) {
    static const ErrorCategory category;
    return std::error_code(static_cast<int>(errorCode), category);
}

bool Error::is(std::error_code targetErrorCode) const {
    return as(targetErrorCode) != nullptr;
}

const Error* Error::as(std::error_code targetErrorCode) const {
    for (const Error* current = this; current != nullptr; current = current->cause.get()) {
        if (current->errorCode == targetErrorCode) {
            return current;
        }
    }
    return nullptr;
}

std::string Error::toString() const {
    std::string text;
    for (const Error* current = this; current != nullptr; current = current->cause.get()) {
        if (!text.empty()) {
            text += ": ";
        }
        if (!current->errorMessage.empty()) {
            text += current->errorMessage;
        } else {
            text += current->errorCode.message();
        }
    }
    for (const Error* current = this; current != nullptr; current = current->cause.get()) {
        for (const std::tuple<std::string, std::uint_least32_t, std::string>& frame : current->stacktrace) {
            const std::string& file = std::get<0>(frame);
            const std::uint_least32_t line = std::get<1>(frame);
            const std::string& description = std::get<2>(frame);
            text += "\n  at " + file + ":" + std::to_string(line);
            if (!description.empty()) {
                text += " " + description;
            }
        }
    }
    return text;
}

Error message(std::string errorMessage) {
    return Error{
        .errorCode = ErrorCode::GenericError,
        .errorMessage = std::move(errorMessage),
    };
}

Error withStackTrace(Error error) {
    for (const Error* current = &error; current != nullptr; current = current->cause.get()) {
        if (!current->stacktrace.empty()) {
            return error;
        }
    }
    const cpptrace::stacktrace stacktrace = cpptrace::generate_trace(1, 30);
    for (const cpptrace::stacktrace_frame& frame : stacktrace.frames) {
        if (error.stacktrace.size() >= 30) {
            break;
        }
        error.stacktrace.push_back({frame.filename, frame.line.value_or(0), frame.symbol});
    }
    return error;
}

} // namespace amj::errors
