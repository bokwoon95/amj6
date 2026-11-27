#include "quickjs.hpp"

#include "safe.hpp"

#include <quickjs.h>

#include <array>
#include <exception>
#include <expected>
#include <iostream>
#include <source_location>
#include <system_error>
#include <tuple>
#include <utility>

namespace amj::safe {

std::expected<QuickJSRuntime, std::error_code> QuickJSRuntime::create() noexcept {
    QuickJSRuntime quickJSRuntime;
    quickJSRuntime.jsRuntime = JS_NewRuntime();
    if (quickJSRuntime.jsRuntime == nullptr) {
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }
    return quickJSRuntime;
}

QuickJSRuntime::~QuickJSRuntime() noexcept {
    const std::error_code errorCode = close();
    if (errorCode) {
        print(std::cerr, std::to_array<PrintArg>({std::source_location::current(),
                             ": QuickJSRuntime.close: ", errorCode, "\n"}));
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
}

std::error_code QuickJSRuntime::close(std::source_location sourceLocation) noexcept {
    if (!borrowers.empty()) {
        print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                             ": closed QuickJSRuntime with outstanding borrowers:\n"}));
        for (const Borrower& borrower : borrowers) {
            print(std::cerr, std::to_array<PrintArg>({"- ", borrower.getSourceLocation(), "\n"}));
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

JSRuntime* QuickJSRuntime::get(std::source_location sourceLocation) const noexcept {
    if (jsRuntime == nullptr) {
        print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                             ": QuickJSRuntime is closed or moved-from\n"}));
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
    return jsRuntime;
}

Borrowed<QuickJSRuntime> QuickJSRuntime::borrow(std::source_location sourceLocation) noexcept {
    if (jsRuntime == nullptr) {
        print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                             ": QuickJSRuntime is closed or moved-from\n"}));
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
    return Borrowed<QuickJSRuntime>(sourceLocation, borrowers, jsRuntime);
}

std::expected<QuickJSContext, std::error_code>
QuickJSContext::create(Borrowed<QuickJSRuntime> quickJSRuntime) noexcept {
    QuickJSContext quickJSContext;
    quickJSContext.jsContext = JS_NewContext(quickJSRuntime.get());
    if (quickJSContext.jsContext == nullptr) {
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }
    quickJSContext.quickJSRuntime = std::move(quickJSRuntime);
    return std::expected<QuickJSContext, std::error_code>(std::in_place, std::move(quickJSContext));
}

QuickJSContext::~QuickJSContext() noexcept {
    const std::error_code errorCode = close();
    if (errorCode) {
        print(std::cerr, std::to_array<PrintArg>({std::source_location::current(),
                             ": QuickJSContext.close: ", errorCode, "\n"}));
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
}

std::error_code QuickJSContext::close(std::source_location sourceLocation) noexcept {
    if (!borrowers.empty()) {
        print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                             ": closed QuickJSContext with outstanding borrowers:\n"}));
        for (const Borrower& borrower : borrowers) {
            print(std::cerr, std::to_array<PrintArg>({"- ", borrower.getSourceLocation(), "\n"}));
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

JSContext* QuickJSContext::get(std::source_location sourceLocation) const noexcept {
    if (jsContext == nullptr) {
        print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                             ": QuickJSContext is closed or moved-from\n"}));
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
    return jsContext;
}

Borrowed<QuickJSContext> QuickJSContext::borrow(std::source_location sourceLocation) noexcept {
    if (jsContext == nullptr) {
        print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                             ": QuickJSContext is closed or moved-from\n"}));
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
    return Borrowed<QuickJSContext>(sourceLocation, borrowers, jsContext);
}

QuickJSValue::~QuickJSValue() noexcept {
    const std::error_code errorCode = close();
    if (errorCode) {
        const std::source_location sourceLocation = std::source_location::current();
        print(std::cerr,
            std::to_array<PrintArg>({sourceLocation, " QuickJS value cleanup failed\n"}));
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
}

JSValue QuickJSValue::get(std::source_location sourceLocation) const noexcept {
    if (!live) {
        print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                             " Access through a closed or moved QuickJS value\n"}));
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
    return jsValue;
}

Borrowed<QuickJSValue> QuickJSValue::borrow(std::source_location sourceLocation) noexcept {
    if (!live) {
        print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                             " Borrowing a closed or moved QuickJS value\n"}));
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
    return Borrowed<QuickJSValue>(sourceLocation, borrowers, jsValue);
}

JSValue QuickJSValue::release(std::source_location sourceLocation) noexcept {
    const JSValue result = get(sourceLocation);
    if (!borrowers.empty()) {
        print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                             " Releasing a QuickJS value with outstanding borrowers\n"}));
        for (const Borrower& borrower : borrowers) {
            print(std::cerr, std::to_array<PrintArg>({"- ", borrower.getSourceLocation(), "\n"}));
        }
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
    jsValue = JS_UNDEFINED;
    live = false;
    quickJSContext.reset();
    return result;
}

std::error_code QuickJSValue::close(std::source_location sourceLocation) noexcept {
    if (!borrowers.empty()) {
        print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                             " Closing a QuickJS value with outstanding borrowers\n"}));
        for (const Borrower& borrower : borrowers) {
            print(std::cerr, std::to_array<PrintArg>({"- ", borrower.getSourceLocation(), "\n"}));
        }
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
    if (!live) {
        return {};
    }
    live = false;
    const JSValue releasedValue = std::exchange(jsValue, JS_UNDEFINED);
    if (!quickJSContext) {
        print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                             " QuickJS value has no context during cleanup\n"}));
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
    try {
        JS_FreeValue(quickJSContext.get(sourceLocation), releasedValue);
    } catch (...) {
        print(std::cerr,
            std::to_array<PrintArg>({sourceLocation, " QuickJS value cleanup failed\n"}));
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
    quickJSContext.reset();
    return {};
}

} // namespace amj::safe
