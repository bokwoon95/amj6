#include "quickjs.hpp"

#include "safe.hpp"

#include <quickjs.h>

#include <array>
#include <exception>
#include <expected>
#include <iostream>
#include <source_location>
#include <system_error>
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
    if (quickJSContext) {
        JS_FreeValue(quickJSContext.get(), jsValue);
    }
}

QuickJSValue::QuickJSValue(const QuickJSValue& other) noexcept {
    if (other.quickJSContext) {
        quickJSContext = other.quickJSContext.borrow();
        jsValue = JS_DupValue(quickJSContext.get(), other.jsValue);
    }
}

QuickJSValue& QuickJSValue::operator=(const QuickJSValue& other) noexcept {
    if (this == &other) {
        return *this;
    }
    Borrowed<QuickJSContext> borrowedQuickJSContext;
    JSValue duplicatedJSValue = JS_UNDEFINED;
    if (other.quickJSContext) {
        borrowedQuickJSContext = other.quickJSContext.borrow();
        duplicatedJSValue = JS_DupValue(borrowedQuickJSContext.get(), other.jsValue);
    }
    if (quickJSContext) {
        JS_FreeValue(quickJSContext.get(), jsValue);
    }
    quickJSContext = std::move(borrowedQuickJSContext);
    jsValue = duplicatedJSValue;
    return *this;
}

QuickJSValue& QuickJSValue::operator=(QuickJSValue&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    if (quickJSContext) {
        JS_FreeValue(quickJSContext.get(), jsValue);
    }
    quickJSContext = std::move(other.quickJSContext);
    jsValue = std::exchange(other.jsValue, JS_UNDEFINED);
    return *this;
}

void QuickJSValue::reset() noexcept {
    if (quickJSContext) {
        JS_FreeValue(quickJSContext.get(), jsValue);
    }
    jsValue = JS_UNDEFINED;
    quickJSContext.reset();
}

} // namespace amj::safe
