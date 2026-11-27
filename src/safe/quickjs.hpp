#pragma once

#include "safe.hpp"

#include <quickjs.h>

#include <exception>
#include <expected>
#include <iostream>
#include <optional>
#include <source_location>
#include <system_error>
#include <tuple>
#include <utility>

namespace amj::safe {

class QuickJSRuntime {
public:
    [[nodiscard]] static std::expected<QuickJSRuntime, std::error_code> create() noexcept {
        QuickJSRuntime quickJSRuntime;
        quickJSRuntime.jsRuntime = JS_NewRuntime();
        if (quickJSRuntime.jsRuntime == nullptr) {
            return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
        }
        return quickJSRuntime;
    }

    ~QuickJSRuntime() noexcept {
        const std::error_code errorCode = close();
        if (errorCode) {
            const std::source_location sourceLocation = std::source_location::current();
            try {
                std::cerr << "QuickJS runtime cleanup failed at " << sourceLocation.file_name()
                          << ':' << sourceLocation.line() << " (" << sourceLocation.function_name()
                          << ')' << '\n'
                          << std::flush;
            } catch (...) {
                printStackTrace(std::cerr, 1, 30);
                std::terminate();
            }
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
    }
    QuickJSRuntime(const QuickJSRuntime&) = delete;
    QuickJSRuntime& operator=(const QuickJSRuntime&) = delete;
    QuickJSRuntime(QuickJSRuntime&& other) noexcept
        : jsRuntime(std::exchange(other.jsRuntime, nullptr)), head(std::move(other.head)) {}
    QuickJSRuntime& operator=(QuickJSRuntime&&) = delete;

    [[nodiscard]] JSRuntime*
    get(std::source_location sourceLocation = std::source_location::current()) const noexcept {
        if (jsRuntime == nullptr) {
            try {
                std::cerr << "Access through a closed or moved QuickJS runtime at "
                          << sourceLocation.file_name() << ':' << sourceLocation.line() << " ("
                          << sourceLocation.function_name() << ')' << '\n'
                          << std::flush;
            } catch (...) {
                printStackTrace(std::cerr, 1, 30);
                std::terminate();
            }
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
        return jsRuntime;
    }

    [[nodiscard]] Borrowed<QuickJSRuntime>
    borrow(std::source_location sourceLocation = std::source_location::current()) noexcept {
        if (jsRuntime == nullptr) {
            try {
                std::cerr << "Borrowing a closed or moved QuickJS runtime at "
                          << sourceLocation.file_name() << ':' << sourceLocation.line() << " ("
                          << sourceLocation.function_name() << ')' << '\n'
                          << std::flush;
            } catch (...) {
                printStackTrace(std::cerr, 1, 30);
                std::terminate();
            }
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
        return Borrowed<QuickJSRuntime>(sourceLocation, head, jsRuntime);
    }

    [[nodiscard]] std::error_code
    close(std::source_location sourceLocation = std::source_location::current()) noexcept {
        if (!head.empty()) {
            try {
                std::cerr << "Closing a QuickJS runtime with outstanding borrowers at "
                          << sourceLocation.file_name() << ':' << sourceLocation.line() << " ("
                          << sourceLocation.function_name() << ')' << '\n'
                          << std::flush;
                for (const Borrower& borrower : head) {
                    std::cerr << borrower.toString() << '\n' << std::flush;
                }
            } catch (...) {
                printStackTrace(std::cerr, 1, 30);
                std::terminate();
            }
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
        JSRuntime* runtime = std::exchange(jsRuntime, nullptr);
        if (runtime != nullptr) {
            JS_FreeRuntime(runtime);
        }
        return {};
    }

private:
    QuickJSRuntime() noexcept = default;

    JSRuntime* jsRuntime = nullptr;
    Borrower head = {};
};

class QuickJSContext {
public:
    [[nodiscard]] static std::expected<QuickJSContext, std::error_code>
    create(Borrowed<QuickJSRuntime> quickJSRuntime) noexcept {
        QuickJSContext quickJSContext;
        quickJSContext.jsContext = JS_NewContext(quickJSRuntime.get());
        if (quickJSContext.jsContext == nullptr) {
            return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
        }
        quickJSContext.quickJSRuntime.emplace(std::move(quickJSRuntime));
        return std::expected<QuickJSContext, std::error_code>(std::in_place,
            std::move(quickJSContext));
    }

    ~QuickJSContext() noexcept {
        const std::error_code errorCode = close();
        if (errorCode) {
            const std::source_location sourceLocation = std::source_location::current();
            try {
                std::cerr << "QuickJS context cleanup failed at " << sourceLocation.file_name()
                          << ':' << sourceLocation.line() << " (" << sourceLocation.function_name()
                          << ')' << '\n'
                          << std::flush;
            } catch (...) {
                printStackTrace(std::cerr, 1, 30);
                std::terminate();
            }
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
    }
    QuickJSContext(const QuickJSContext&) = delete;
    QuickJSContext& operator=(const QuickJSContext&) = delete;
    QuickJSContext(QuickJSContext&& other) noexcept
        : quickJSRuntime(std::move(other.quickJSRuntime)),
          jsContext(std::exchange(other.jsContext, nullptr)), head(std::move(other.head)) {
        other.quickJSRuntime.reset();
    }
    QuickJSContext& operator=(QuickJSContext&&) = delete;

    [[nodiscard]] JSContext*
    get(std::source_location sourceLocation = std::source_location::current()) const noexcept {
        if (jsContext == nullptr) {
            try {
                std::cerr << "Access through a closed or moved QuickJS context at "
                          << sourceLocation.file_name() << ':' << sourceLocation.line() << " ("
                          << sourceLocation.function_name() << ')' << '\n'
                          << std::flush;
            } catch (...) {
                printStackTrace(std::cerr, 1, 30);
                std::terminate();
            }
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
        return jsContext;
    }

    [[nodiscard]] Borrowed<QuickJSContext>
    borrow(std::source_location sourceLocation = std::source_location::current()) noexcept {
        if (jsContext == nullptr) {
            try {
                std::cerr << "Borrowing a closed or moved QuickJS context at "
                          << sourceLocation.file_name() << ':' << sourceLocation.line() << " ("
                          << sourceLocation.function_name() << ')' << '\n'
                          << std::flush;
            } catch (...) {
                printStackTrace(std::cerr, 1, 30);
                std::terminate();
            }
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
        return Borrowed<QuickJSContext>(sourceLocation, head, jsContext);
    }

    [[nodiscard]] std::error_code
    close(std::source_location sourceLocation = std::source_location::current()) noexcept {
        if (!head.empty()) {
            try {
                std::cerr << "Closing a QuickJS context with outstanding borrowers at "
                          << sourceLocation.file_name() << ':' << sourceLocation.line() << " ("
                          << sourceLocation.function_name() << ')' << '\n'
                          << std::flush;
                for (const Borrower& borrower : head) {
                    std::cerr << borrower.toString() << '\n' << std::flush;
                }
            } catch (...) {
                printStackTrace(std::cerr, 1, 30);
                std::terminate();
            }
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
        JSContext* context = std::exchange(jsContext, nullptr);
        if (context != nullptr) {
            JS_FreeContext(context);
            quickJSRuntime.reset();
        }
        return {};
    }

private:
    QuickJSContext() noexcept = default;

    std::optional<Borrowed<QuickJSRuntime>> quickJSRuntime = std::nullopt;
    JSContext* jsContext = nullptr;
    Borrower head = {};
};

// Adopts one owned JSValue reference. Call JS_DupValue explicitly when starting
// from a borrowed value. The context borrow remains registered through cleanup.
class QuickJSValue {
public:
    QuickJSValue(Borrowed<QuickJSContext> quickJSContext, JSValue value) noexcept
        : value(value), live(true) {
        // Validate the dependency before transferring its registration.
        std::ignore = quickJSContext.get();
        this->quickJSContext.emplace(std::move(quickJSContext));
    }

    ~QuickJSValue() noexcept {
        const std::error_code errorCode = close();
        if (errorCode) {
            const std::source_location sourceLocation = std::source_location::current();
            try {
                std::cerr << "QuickJS value cleanup failed at " << sourceLocation.file_name() << ':'
                          << sourceLocation.line() << " (" << sourceLocation.function_name() << ')'
                          << '\n'
                          << std::flush;
            } catch (...) {
                printStackTrace(std::cerr, 1, 30);
                std::terminate();
            }
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
    }
    QuickJSValue(const QuickJSValue&) = delete;
    QuickJSValue& operator=(const QuickJSValue&) = delete;
    QuickJSValue(QuickJSValue&& other) noexcept
        : quickJSContext(std::move(other.quickJSContext)),
          value(std::exchange(other.value, JS_UNDEFINED)), live(std::exchange(other.live, false)),
          head(std::move(other.head)) {
        other.quickJSContext.reset();
    }
    QuickJSValue& operator=(QuickJSValue&&) = delete;

    [[nodiscard]] JSValue
    get(std::source_location sourceLocation = std::source_location::current()) const noexcept {
        if (!live) {
            try {
                std::cerr << "Access through a closed or moved QuickJS value at "
                          << sourceLocation.file_name() << ':' << sourceLocation.line() << " ("
                          << sourceLocation.function_name() << ')' << '\n'
                          << std::flush;
            } catch (...) {
                printStackTrace(std::cerr, 1, 30);
                std::terminate();
            }
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
        return value;
    }

    [[nodiscard]] Borrowed<QuickJSValue>
    borrow(std::source_location sourceLocation = std::source_location::current()) noexcept {
        if (!live) {
            try {
                std::cerr << "Borrowing a closed or moved QuickJS value at "
                          << sourceLocation.file_name() << ':' << sourceLocation.line() << " ("
                          << sourceLocation.function_name() << ')' << '\n'
                          << std::flush;
            } catch (...) {
                printStackTrace(std::cerr, 1, 30);
                std::terminate();
            }
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
        return Borrowed<QuickJSValue>(sourceLocation, head, value);
    }

    [[nodiscard]] JSValue
    release(std::source_location sourceLocation = std::source_location::current()) noexcept {
        const JSValue result = get(sourceLocation);
        if (!head.empty()) {
            try {
                std::cerr << "Releasing a QuickJS value with outstanding borrowers at "
                          << sourceLocation.file_name() << ':' << sourceLocation.line() << " ("
                          << sourceLocation.function_name() << ')' << '\n'
                          << std::flush;
                for (const Borrower& borrower : head) {
                    std::cerr << borrower.toString() << '\n' << std::flush;
                }
            } catch (...) {
                printStackTrace(std::cerr, 1, 30);
                std::terminate();
            }
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
        value = JS_UNDEFINED;
        live = false;
        quickJSContext.reset();
        return result;
    }

    [[nodiscard]] std::error_code
    close(std::source_location sourceLocation = std::source_location::current()) noexcept {
        if (!head.empty()) {
            try {
                std::cerr << "Closing a QuickJS value with outstanding borrowers at "
                          << sourceLocation.file_name() << ':' << sourceLocation.line() << " ("
                          << sourceLocation.function_name() << ')' << '\n'
                          << std::flush;
                for (const Borrower& borrower : head) {
                    std::cerr << borrower.toString() << '\n' << std::flush;
                }
            } catch (...) {
                printStackTrace(std::cerr, 1, 30);
                std::terminate();
            }
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
        if (!live) {
            return {};
        }
        live = false;
        const JSValue releasedValue = std::exchange(value, JS_UNDEFINED);
        if (!quickJSContext.has_value()) {
            try {
                std::cerr << "QuickJS value has no context during cleanup at "
                          << sourceLocation.file_name() << ':' << sourceLocation.line() << " ("
                          << sourceLocation.function_name() << ')' << '\n'
                          << std::flush;
            } catch (...) {
                printStackTrace(std::cerr, 1, 30);
                std::terminate();
            }
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
        try {
            JS_FreeValue(quickJSContext.value().get(sourceLocation), releasedValue);
        } catch (...) {
            try {
                std::cerr << "QuickJS value cleanup failed at " << sourceLocation.file_name() << ':'
                          << sourceLocation.line() << " (" << sourceLocation.function_name() << ')'
                          << '\n'
                          << std::flush;
            } catch (...) {
                printStackTrace(std::cerr, 1, 30);
                std::terminate();
            }
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
        quickJSContext.reset();
        return {};
    }

private:
    std::optional<Borrowed<QuickJSContext>> quickJSContext = std::nullopt;
    JSValue value = JS_UNDEFINED;
    bool live = false;
    Borrower head = {};
};

struct QuickJSPromise {
    std::optional<QuickJSValue> promise = std::nullopt;
    std::optional<QuickJSValue> resolveFunction = std::nullopt;
    std::optional<QuickJSValue> rejectFunction = std::nullopt;
};

} // namespace amj::safe
