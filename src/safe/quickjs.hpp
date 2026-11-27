#pragma once

#include "safe.hpp"

#include <quickjs.h>

#include <array>
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
            print(std::cerr, std::to_array<PrintArg>({std::source_location::current(),
                                 ": QuickJSRuntime.close: ", errorCode, "\n"}));
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
    }
    QuickJSRuntime(const QuickJSRuntime&) = delete;
    QuickJSRuntime& operator=(const QuickJSRuntime&) = delete;
    QuickJSRuntime(QuickJSRuntime&& other) noexcept
        : jsRuntime(std::exchange(other.jsRuntime, nullptr)),
          borrowers(std::move(other.borrowers)) {}
    QuickJSRuntime& operator=(QuickJSRuntime&&) = delete;

    [[nodiscard]] std::error_code
    close(std::source_location sourceLocation = std::source_location::current()) noexcept {
        if (!borrowers.empty()) {
            print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                                 ": closed QuickJSRuntime with outstanding borrowers:\n"}));
            for (const Borrower& borrower : borrowers) {
                print(std::cerr, std::to_array<PrintArg>({"- ", borrower.sourceLocation, "\n"}));
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

    [[nodiscard]] JSRuntime*
    get(std::source_location sourceLocation = std::source_location::current()) const noexcept {
        if (jsRuntime == nullptr) {
            print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                                 ": QuickJSRuntime is closed or moved-from\n"}));
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
        return jsRuntime;
    }

    [[nodiscard]] Borrowed<QuickJSRuntime>
    borrow(std::source_location sourceLocation = std::source_location::current()) noexcept {
        if (jsRuntime == nullptr) {
            print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                                 ": QuickJSRuntime is closed or moved-from\n"}));
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
        return Borrowed<QuickJSRuntime>(sourceLocation, borrowers, jsRuntime);
    }

private:
    QuickJSRuntime() noexcept = default;

    JSRuntime* jsRuntime = nullptr;
    Borrower borrowers = {};
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
            print(std::cerr, std::to_array<PrintArg>({std::source_location::current(),
                                 ": QuickJSContext.close: ", errorCode, "\n"}));
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
    }
    QuickJSContext(const QuickJSContext&) = delete;
    QuickJSContext& operator=(const QuickJSContext&) = delete;
    QuickJSContext(QuickJSContext&& other) noexcept
        : quickJSRuntime(std::move(other.quickJSRuntime)),
          jsContext(std::exchange(other.jsContext, nullptr)),
          borrowers(std::move(other.borrowers)) {
        other.quickJSRuntime.reset();
    }
    QuickJSContext& operator=(QuickJSContext&&) = delete;

    [[nodiscard]] std::error_code
    close(std::source_location sourceLocation = std::source_location::current()) noexcept {
        if (!borrowers.empty()) {
            print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                                 ": closed QuickJSContext with outstanding borrowers:\n"}));
            for (const Borrower& borrower : borrowers) {
                print(std::cerr, std::to_array<PrintArg>({"- ", borrower.sourceLocation, "\n"}));
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

    [[nodiscard]] JSContext*
    get(std::source_location sourceLocation = std::source_location::current()) const noexcept {
        if (jsContext == nullptr) {
            print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                                 ": QuickJSContext is closed or moved-from\n"}));
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
        return jsContext;
    }

    [[nodiscard]] Borrowed<QuickJSContext>
    borrow(std::source_location sourceLocation = std::source_location::current()) noexcept {
        if (jsContext == nullptr) {
            print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                                 ": QuickJSContext is closed or moved-from\n"}));
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
        return Borrowed<QuickJSContext>(sourceLocation, borrowers, jsContext);
    }

private:
    QuickJSContext() noexcept = default;

    std::optional<Borrowed<QuickJSRuntime>> quickJSRuntime = std::nullopt;
    JSContext* jsContext = nullptr;
    Borrower borrowers = {};
};

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
            print(std::cerr,
                std::to_array<PrintArg>({sourceLocation, " QuickJS value cleanup failed\n"}));
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
            print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                                 " Access through a closed or moved QuickJS value\n"}));
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
        return value;
    }

    [[nodiscard]] Borrowed<QuickJSValue>
    borrow(std::source_location sourceLocation = std::source_location::current()) noexcept {
        if (!live) {
            print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                                 " Borrowing a closed or moved QuickJS value\n"}));
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
        return Borrowed<QuickJSValue>(sourceLocation, head, value);
    }

    [[nodiscard]] JSValue
    release(std::source_location sourceLocation = std::source_location::current()) noexcept {
        const JSValue result = get(sourceLocation);
        if (!head.empty()) {
            print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                                 " Releasing a QuickJS value with outstanding borrowers\n"}));
            for (const Borrower& borrower : head) {
                print(std::cerr, std::to_array<PrintArg>({"- ", borrower.sourceLocation, "\n"}));
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
            print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                                 " Closing a QuickJS value with outstanding borrowers\n"}));
            for (const Borrower& borrower : head) {
                print(std::cerr, std::to_array<PrintArg>({"- ", borrower.sourceLocation, "\n"}));
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
            print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                                 " QuickJS value has no context during cleanup\n"}));
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
        try {
            JS_FreeValue(quickJSContext.value().get(sourceLocation), releasedValue);
        } catch (...) {
            print(std::cerr,
                std::to_array<PrintArg>({sourceLocation, " QuickJS value cleanup failed\n"}));
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
