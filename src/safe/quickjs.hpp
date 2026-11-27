#pragma once

#include "safe.hpp"

#include <quickjs.h>

#include <expected>
#include <source_location>
#include <system_error>
#include <tuple>
#include <utility>

namespace amj::safe {

class QuickJSRuntime {
public:
    [[nodiscard]] static std::expected<QuickJSRuntime, std::error_code> create() noexcept;

    ~QuickJSRuntime() noexcept;
    QuickJSRuntime(const QuickJSRuntime&) = delete;
    QuickJSRuntime& operator=(const QuickJSRuntime&) = delete;
    QuickJSRuntime(QuickJSRuntime&& other) noexcept
        : jsRuntime(std::exchange(other.jsRuntime, nullptr)),
          borrowers(std::move(other.borrowers)) {}
    QuickJSRuntime& operator=(QuickJSRuntime&&) = delete;

    [[nodiscard]] std::error_code
    close(std::source_location sourceLocation = std::source_location::current()) noexcept;

    [[nodiscard]] JSRuntime*
    get(std::source_location sourceLocation = std::source_location::current()) const noexcept;

    [[nodiscard]] Borrowed<QuickJSRuntime>
    borrow(std::source_location sourceLocation = std::source_location::current()) noexcept;

private:
    QuickJSRuntime() noexcept = default;

    JSRuntime* jsRuntime = nullptr;
    Borrower borrowers = {};
};

class QuickJSContext {
public:
    [[nodiscard]] static std::expected<QuickJSContext, std::error_code>
    create(Borrowed<QuickJSRuntime> quickJSRuntime) noexcept;

    ~QuickJSContext() noexcept;
    QuickJSContext(const QuickJSContext&) = delete;
    QuickJSContext& operator=(const QuickJSContext&) = delete;
    QuickJSContext(QuickJSContext&& other) noexcept
        : quickJSRuntime(std::move(other.quickJSRuntime)),
          jsContext(std::exchange(other.jsContext, nullptr)),
          borrowers(std::move(other.borrowers)) {}
    QuickJSContext& operator=(QuickJSContext&&) = delete;

    [[nodiscard]] std::error_code
    close(std::source_location sourceLocation = std::source_location::current()) noexcept;

    [[nodiscard]] JSContext*
    get(std::source_location sourceLocation = std::source_location::current()) const noexcept;

    [[nodiscard]] Borrowed<QuickJSContext>
    borrow(std::source_location sourceLocation = std::source_location::current()) noexcept;

private:
    QuickJSContext() noexcept = default;

    Borrowed<QuickJSRuntime> quickJSRuntime = {};
    JSContext* jsContext = nullptr;
    Borrower borrowers = {};
};

class QuickJSValue {
public:
    QuickJSValue() noexcept = default;

    QuickJSValue(Borrowed<QuickJSContext> quickJSContext, JSValue jsValue) noexcept
        : jsValue(jsValue) {
        std::ignore = quickJSContext.get(); // Make sure quickJSContext is valid.
        this->quickJSContext = std::move(quickJSContext);
    };

    ~QuickJSValue() noexcept;
    QuickJSValue(const QuickJSValue& other) noexcept;
    QuickJSValue& operator=(const QuickJSValue& other) noexcept;
    QuickJSValue(QuickJSValue&& other) noexcept
        : quickJSContext(std::move(other.quickJSContext)),
          jsValue(std::exchange(other.jsValue, JS_UNDEFINED)) {}
    QuickJSValue& operator=(QuickJSValue&& other) noexcept;

    [[nodiscard]] JSValue get() const noexcept {
        return jsValue;
    }

    [[nodiscard]] JSValue release() noexcept {
        const JSValue releasedValue = std::exchange(jsValue, JS_UNDEFINED);
        quickJSContext.reset();
        return releasedValue;
    }

    void reset() noexcept;

private:
    Borrowed<QuickJSContext> quickJSContext = {};
    JSValue jsValue = JS_UNDEFINED;
};

struct QuickJSPromise {
    QuickJSValue promise = {};
    QuickJSValue resolveFunction = {};
    QuickJSValue rejectFunction = {};
};

} // namespace amj::safe
