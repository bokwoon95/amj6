#pragma once

#include "safe.hpp"

#include <quickjs.h>

#include <expected>
#include <optional>
#include <source_location>
#include <system_error>
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
    QuickJSValue(Borrowed<QuickJSContext> quickJSContext, JSValue jsValue) noexcept
        : jsValue(jsValue), live(true) {
        std::ignore = quickJSContext.get(); // Make sure quickJSContext is valid.
        this->quickJSContext = std::move(quickJSContext);
    };

    ~QuickJSValue() noexcept;
    QuickJSValue(const QuickJSValue&) = delete;
    QuickJSValue& operator=(const QuickJSValue&) = delete;
    QuickJSValue(QuickJSValue&& other) noexcept
        : quickJSContext(std::move(other.quickJSContext)),
          jsValue(std::exchange(other.jsValue, JS_UNDEFINED)),
          live(std::exchange(other.live, false)), borrowers(std::move(other.borrowers)) {}
    QuickJSValue& operator=(QuickJSValue&&) = delete;

    [[nodiscard]] JSValue
    get(std::source_location sourceLocation = std::source_location::current()) const noexcept;

    [[nodiscard]] Borrowed<QuickJSValue>
    borrow(std::source_location sourceLocation = std::source_location::current()) noexcept;

    [[nodiscard]] JSValue
    release(std::source_location sourceLocation = std::source_location::current()) noexcept;

    [[nodiscard]] std::error_code
    close(std::source_location sourceLocation = std::source_location::current()) noexcept;

private:
    Borrowed<QuickJSContext> quickJSContext = {};
    JSValue jsValue = JS_UNDEFINED;
    bool live = false;
    Borrower borrowers = {};
};

struct QuickJSPromise {
    std::optional<QuickJSValue> promise = std::nullopt;
    std::optional<QuickJSValue> resolveFunction = std::nullopt;
    std::optional<QuickJSValue> rejectFunction = std::nullopt;
};

} // namespace amj::safe
